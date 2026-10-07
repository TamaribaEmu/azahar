// Copyright 2016 Citra Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include "common/archives.h"
#include "common/logging/log.h"
#include "core/core.h"
#include "core/hle/ipc_helpers.h"
#include "core/hle/kernel/process.h"
#include "core/hle/service/mvd/mvd_std.h"

#ifdef AZAHAR_MVD_FFMPEG
#include <algorithm>
#include <cerrno>
#include <cstring>
#include <limits>
#include <vector>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/frame.h>
#include <libavutil/mem.h>
#include <libswscale/swscale.h>
}

#include "core/memory.h"
#include "video_core/gpu.h"
#endif

SERIALIZE_EXPORT_IMPL(Service::MVD::MVD_STD)
SERVICE_CONSTRUCT_IMPL(Service::MVD::MVD_STD)

namespace Service::MVD {

namespace {
constexpr u32 MvdStatusOk = 0x17000;
#ifdef AZAHAR_MVD_FFMPEG
constexpr u32 MvdStatusParameterSet = 0x17001;
constexpr u32 MvdStatusFrameReady = 0x17003;
constexpr u32 MvdInvalidConfiguration = 0xD961710F;
constexpr u32 DefaultWorkBufferSize = 0x9006C8;
constexpr u32 MaxNalUnitSize = 32 * 1024 * 1024;
constexpr u32 InputH264 = 0x00020001;
constexpr u32 OutputYuyv422 = 0x00010001;
constexpr u32 OutputBgr565 = 0x00040002;
constexpr u32 OutputRgb565 = 0x00040004;

u8 GetNalUnitType(const u8* data, std::size_t size) {
    std::size_t offset = 0;
    while (offset + 3 < size && data[offset] == 0) {
        ++offset;
    }
    if (offset < size && data[offset] == 1) {
        ++offset;
    }
    return offset < size ? data[offset] & 0x1F : 0;
}

PAddr LinearVirtualToPhysical(VAddr address) {
    if (address >= Memory::NEW_LINEAR_HEAP_VADDR &&
        address < Memory::NEW_LINEAR_HEAP_VADDR_END) {
        return Memory::FCRAM_PADDR + address - Memory::NEW_LINEAR_HEAP_VADDR;
    }
    if (address >= Memory::LINEAR_HEAP_VADDR && address < Memory::LINEAR_HEAP_VADDR_END) {
        return Memory::FCRAM_PADDR + address - Memory::LINEAR_HEAP_VADDR;
    }
    return 0;
}
#endif
} // namespace

#ifdef AZAHAR_MVD_FFMPEG
class MVD_STD::Decoder {
public:
    Decoder() = default;
    ~Decoder() {
        Reset();
    }

    u32 Decode(const u8* data, std::size_t size, u32 sequence, u32 width, u32 height) {
        const u8 nal_type = GetNalUnitType(data, size);
        const bool parameter_set = nal_type == 7 || nal_type == 8;
        if (nal_type == 7) {
            sps.assign(data, data + size);
        } else if (nal_type == 8) {
            pps.assign(data, data + size);
        }

        if (!context && parameter_set && (sps.empty() || pps.empty())) {
            return MvdStatusParameterSet;
        }

        if (!context) {
            const std::vector<u8> extradata = ParameterSetExtradata();
            if ((extradata.empty() || !Open(true, width, height, extradata)) &&
                !Open(false, width, height, {})) {
                return MvdInvalidConfiguration;
            }
            if (!hardware) {
                if (!sps.empty() && SendPacket(sps.data(), sps.size(), 0) < 0) {
                    return MvdInvalidConfiguration;
                }
                if (!pps.empty() && SendPacket(pps.data(), pps.size(), 0) < 0) {
                    return MvdInvalidConfiguration;
                }
            } else if (parameter_set) {
                return MvdStatusParameterSet;
            }
        }

        int result = SendPacket(data, size, sequence);
        if (result < 0 && hardware) {
            LOG_WARNING(Service, "MediaCodec rejected an H.264 packet; using FFmpeg software "
                                 "decoding for this MVD session");
            if (!Open(false, width, height, {})) {
                return MvdInvalidConfiguration;
            }
            if (!sps.empty() && SendPacket(sps.data(), sps.size(), 0) < 0) {
                return MvdInvalidConfiguration;
            }
            if (!pps.empty() && SendPacket(pps.data(), pps.size(), 0) < 0) {
                return MvdInvalidConfiguration;
            }
            result = SendPacket(data, size, sequence);
        }
        if (result < 0) {
            return MvdInvalidConfiguration;
        }

        av_frame_unref(frame);
        result = avcodec_receive_frame(context, frame);
        if (result >= 0) {
            has_frame = true;
            return MvdStatusFrameReady;
        }
        if (result != AVERROR(EAGAIN) && result != AVERROR_EOF) {
            return MvdInvalidConfiguration;
        }
        return parameter_set ? MvdStatusParameterSet : MvdStatusOk;
    }

    bool Render(u8* output, u32 output_width, u32 output_height, u32 output_stride_pixels,
                u32 output_type) {
        if (!has_frame || !frame || frame->width <= 0 || frame->height <= 0) {
            return false;
        }

        AVPixelFormat destination_format{};
        switch (output_type) {
        case OutputYuyv422:
            destination_format = AV_PIX_FMT_YUYV422;
            break;
        case OutputBgr565:
            destination_format = AV_PIX_FMT_BGR565LE;
            break;
        case OutputRgb565:
            destination_format = AV_PIX_FMT_RGB565LE;
            break;
        default:
            return false;
        }

        swscale = sws_getCachedContext(swscale, frame->width, frame->height,
                                       static_cast<AVPixelFormat>(frame->format), output_width,
                                       output_height, destination_format, SWS_FAST_BILINEAR, nullptr,
                                       nullptr, nullptr);
        if (!swscale) {
            return false;
        }

        uint8_t* destination[] = {output, nullptr, nullptr, nullptr};
        int destination_stride[] = {static_cast<int>(output_stride_pixels * 2), 0, 0, 0};
        const int lines = sws_scale(swscale, frame->data, frame->linesize, 0, frame->height,
                                    destination, destination_stride);
        has_frame = false;
        return lines == static_cast<int>(output_height);
    }

    bool IsHardwareAccelerated() const {
        return hardware;
    }

private:
    bool Open(bool use_hardware, u32 width, u32 height, const std::vector<u8>& extradata) {
        ResetCodec();

        const AVCodec* codec = use_hardware ? avcodec_find_decoder_by_name("h264_mediacodec")
                                            : avcodec_find_decoder(AV_CODEC_ID_H264);
        if (!codec) {
            return false;
        }
        context = avcodec_alloc_context3(codec);
        if (!context) {
            return false;
        }
        context->width = static_cast<int>(width);
        context->height = static_cast<int>(height);
        context->flags |= AV_CODEC_FLAG_LOW_DELAY;
        context->flags2 |= AV_CODEC_FLAG2_CHUNKS;
        context->thread_count = use_hardware ? 1 : 2;
        if (!extradata.empty()) {
            context->extradata = static_cast<u8*>(
                av_mallocz(extradata.size() + AV_INPUT_BUFFER_PADDING_SIZE));
            if (!context->extradata) {
                ResetCodec();
                return false;
            }
            std::memcpy(context->extradata, extradata.data(), extradata.size());
            context->extradata_size = static_cast<int>(extradata.size());
        }
        if (avcodec_open2(context, codec, nullptr) < 0) {
            ResetCodec();
            return false;
        }
        frame = av_frame_alloc();
        if (!frame) {
            ResetCodec();
            return false;
        }
        hardware = use_hardware;
        LOG_INFO(Service, "Opened FFmpeg H.264 decoder '{}' for mvd:std", codec->name);
        return true;
    }

    int SendPacket(const u8* data, std::size_t size, u32 sequence) {
        AVPacket packet{};
        packet.data = const_cast<u8*>(data);
        packet.size = static_cast<int>(size);
        packet.pts = sequence;
        packet.dts = sequence;
        return avcodec_send_packet(context, &packet);
    }

    std::vector<u8> ParameterSetExtradata() const {
        if (sps.empty() || pps.empty()) {
            return {};
        }
        std::vector<u8> extradata;
        extradata.reserve(sps.size() + pps.size());
        extradata.insert(extradata.end(), sps.begin(), sps.end());
        extradata.insert(extradata.end(), pps.begin(), pps.end());
        return extradata;
    }

    void ResetCodec() {
        if (frame) {
            av_frame_free(&frame);
        }
        if (context) {
            avcodec_free_context(&context);
        }
        hardware = false;
        has_frame = false;
    }

    void Reset() {
        ResetCodec();
        if (swscale) {
            sws_freeContext(swscale);
            swscale = nullptr;
        }
    }

    AVCodecContext* context{};
    AVFrame* frame{};
    SwsContext* swscale{};
    std::vector<u8> sps;
    std::vector<u8> pps;
    bool hardware{};
    bool has_frame{};
};
#endif

MVD_STD::MVD_STD(Core::System& system_)
    : ServiceFramework("mvd:std", 1)
#ifdef AZAHAR_MVD_FFMPEG
      , system(system_)
#endif
{
#ifndef AZAHAR_MVD_FFMPEG
    (void)system_;
#endif
    static const FunctionInfo functions[] = {
        // clang-format off
#ifdef AZAHAR_MVD_FFMPEG
        {0x0001, &MVD_STD::Initialize, "Initialize"},
        {0x0002, &MVD_STD::Shutdown, "Shutdown"},
        {0x0003, &MVD_STD::CalculateWorkBufSize, "CalculateWorkBufSize"},
        {0x0004, &MVD_STD::CalculateImageSize, "CalculateImageSize"},
        {0x0008, &MVD_STD::ProcessNALUnit, "ProcessNALUnit"},
        {0x0009, &MVD_STD::ControlFrameRendering, "ControlFrameRendering"},
        {0x000A, &MVD_STD::GetStatus, "GetStatus"},
        {0x000B, &MVD_STD::GetStatusOther, "GetStatusOther"},
        {0x001D, &MVD_STD::GetConfig, "GetConfig"},
        {0x001E, &MVD_STD::SetConfig, "SetConfig"},
        {0x001F, &MVD_STD::SetOutputBuffer, "SetOutputBuffer"},
        {0x0021, &MVD_STD::OverrideOutputBuffers, "OverrideOutputBuffers"}
#else
        {0x0001, nullptr, "Initialize"},
        {0x0002, nullptr, "Shutdown"},
        {0x0003, nullptr, "CalculateWorkBufSize"},
        {0x0004, nullptr, "CalculateImageSize"},
        {0x0008, nullptr, "ProcessNALUnit"},
        {0x0009, nullptr, "ControlFrameRendering"},
        {0x000A, nullptr, "GetStatus"},
        {0x000B, nullptr, "GetStatusOther"},
        {0x001D, nullptr, "GetConfig"},
        {0x001E, nullptr, "SetConfig"},
        {0x001F, nullptr, "SetOutputBuffer"},
        {0x0021, nullptr, "OverrideOutputBuffers"}
#endif
        // clang-format on
    };

    RegisterHandlers(functions);
}

MVD_STD::~MVD_STD() = default;

#ifdef AZAHAR_MVD_FFMPEG
void MVD_STD::Initialize(Kernel::HLERequestContext& ctx) {
    IPC::RequestParser rp(ctx);
    [[maybe_unused]] const u32 work_buffer_address = rp.Pop<u32>();
    [[maybe_unused]] const u32 work_buffer_size = rp.Pop<u32>();
    [[maybe_unused]] auto process = rp.PopObject<Kernel::Process>();

    decoder = std::make_unique<Decoder>();
    initialized = true;
    last_status = MvdStatusOk;

    IPC::RequestBuilder rb = rp.MakeBuilder(1, 0);
    rb.Push(last_status);
    LOG_INFO(Service, "Initialized FFmpeg-backed mvd:std");
}

void MVD_STD::Shutdown(Kernel::HLERequestContext& ctx) {
    IPC::RequestParser rp(ctx);
    decoder.reset();
    initialized = false;
    last_status = MvdStatusOk;

    IPC::RequestBuilder rb = rp.MakeBuilder(1, 0);
    rb.Push(last_status);
}

void MVD_STD::CalculateWorkBufSize(Kernel::HLERequestContext& ctx) {
    IPC::RequestParser rp(ctx);
    [[maybe_unused]] const auto calculation_config = rp.PopRaw<std::array<u32, 12>>();

    IPC::RequestBuilder rb = rp.MakeBuilder(2, 0);
    rb.Push(MvdStatusOk);
    rb.Push(DefaultWorkBufferSize);
}

void MVD_STD::CalculateImageSize(Kernel::HLERequestContext& ctx) {
    IPC::RequestParser rp(ctx);
    const u32 width = rp.Pop<u32>();
    const u32 height = rp.Pop<u32>();
    [[maybe_unused]] const u32 format = rp.Pop<u32>();

    IPC::RequestBuilder rb = rp.MakeBuilder(2, 0);
    rb.Push(MvdStatusOk);
    rb.Push(width <= 8192 && height <= 8192 ? width * height * 2 : 0);
}

void MVD_STD::ProcessNALUnit(Kernel::HLERequestContext& ctx) {
    IPC::RequestParser rp(ctx);
    const VAddr input_address = rp.Pop<u32>();
    const PAddr input_physical_address = rp.Pop<u32>();
    const u32 size = rp.Pop<u32>();
    const u32 sequence = rp.Pop<u32>();
    [[maybe_unused]] const u32 flag = rp.Pop<u32>();
    auto process = rp.PopObject<Kernel::Process>();

    last_status = MvdInvalidConfiguration;
    if (initialized && decoder && process && size > 0 && size <= MaxNalUnitSize &&
        system.Memory().IsValidVirtualAddress(*process, input_address)) {
        std::vector<u8> input(size + AV_INPUT_BUFFER_PADDING_SIZE, 0);
        system.Memory().ReadBlock(*process, input_address, input.data(), size);
        last_status = decoder->Decode(input.data(), size, sequence, config[3], config[4]);
    }

    IPC::RequestBuilder rb = rp.MakeBuilder(4, 0);
    rb.Push(last_status);
    rb.Push(input_address + size);
    rb.Push(input_physical_address + size);
    rb.Push(0);
}

void MVD_STD::ControlFrameRendering(Kernel::HLERequestContext& ctx) {
    IPC::RequestParser rp(ctx);
    const u8 type = rp.Pop<u8>();
    [[maybe_unused]] auto process = rp.PopObject<Kernel::Process>();

    if (type == 0 && decoder) {
        RenderDecodedFrame();
    }
    last_status = MvdStatusOk;

    IPC::RequestBuilder rb = rp.MakeBuilder(1, 0);
    rb.Push(last_status);
}

void MVD_STD::GetStatus(Kernel::HLERequestContext& ctx) {
    IPC::RequestParser rp(ctx);
    IPC::RequestBuilder rb = rp.MakeBuilder(2, 0);
    rb.Push(MvdStatusOk);
    rb.Push(last_status);
}

void MVD_STD::GetStatusOther(Kernel::HLERequestContext& ctx) {
    GetStatus(ctx);
}

void MVD_STD::GetConfig(Kernel::HLERequestContext& ctx) {
    IPC::RequestParser rp(ctx);
    const u32 size = rp.Pop<u32>();
    auto& buffer = rp.PopMappedBuffer();
    buffer.Write(config.data(), 0, std::min<std::size_t>(size, sizeof(config)));

    IPC::RequestBuilder rb = rp.MakeBuilder(1, 2);
    rb.Push(MvdStatusOk);
    rb.PushMappedBuffer(buffer);
}

void MVD_STD::SetConfig(Kernel::HLERequestContext& ctx) {
    IPC::RequestParser rp(ctx);
    const u32 size = rp.Pop<u32>();
    [[maybe_unused]] auto process = rp.PopObject<Kernel::Process>();
    auto& buffer = rp.PopMappedBuffer();

    last_status = MvdInvalidConfiguration;
    if (size >= sizeof(config)) {
        buffer.Read(config.data(), 0, sizeof(config));
        if (config[0] == InputH264 && config[23] > 0 && config[24] > 0 &&
            config[23] <= 8192 && config[24] <= 8192) {
            last_status = MvdStatusOk;
        }
    }

    IPC::RequestBuilder rb = rp.MakeBuilder(1, 2);
    rb.Push(last_status);
    rb.PushMappedBuffer(buffer);
}

void MVD_STD::SetOutputBuffer(Kernel::HLERequestContext& ctx) {
    IPC::RequestParser rp(ctx);
    output_buffer_count = std::min(rp.Pop<u32>(), 17U);
    for (u32 i = 0; i < output_buffers.size(); ++i) {
        output_buffers[i] = rp.Pop<u32>();
    }
    output_buffer_size = rp.Pop<u32>();
    [[maybe_unused]] auto process = rp.PopObject<Kernel::Process>();

    IPC::RequestBuilder rb = rp.MakeBuilder(1, 0);
    rb.Push(MvdStatusOk);
}

void MVD_STD::OverrideOutputBuffers(Kernel::HLERequestContext& ctx) {
    IPC::RequestParser rp(ctx);
    const u32 current0 = rp.Pop<u32>();
    const u32 current1 = rp.Pop<u32>();
    const u32 replacement0 = rp.Pop<u32>();
    const u32 replacement1 = rp.Pop<u32>();

    last_status = MvdInvalidConfiguration;
    if (output_buffer_count > 0 && output_buffers[0] == current0 &&
        output_buffers[1] == current1) {
        output_buffers[0] = replacement0;
        output_buffers[1] = replacement1;
        last_status = MvdStatusOk;
    }

    IPC::RequestBuilder rb = rp.MakeBuilder(1, 0);
    rb.Push(last_status);
}

bool MVD_STD::RenderDecodedFrame() {
    if (!decoder) {
        return false;
    }

    const u32 output_type = config[0x58 / 4];
    const u32 width = config[0x5C / 4];
    const u32 height = config[0x60 / 4];
    const bool use_override = config[0x104 / 4] != 0;
    const u32 x = use_override ? config[0x108 / 4] : 0;
    const u32 y = use_override ? config[0x10C / 4] : 0;
    const u32 canvas_width =
        use_override && config[0x110 / 4] ? config[0x110 / 4] : width;
    const u32 canvas_height =
        use_override && config[0x114 / 4] ? config[0x114 / 4] : height;
    if (width == 0 || height == 0 || canvas_width == 0 || canvas_height == 0 ||
        width > 8192 || height > 8192 || canvas_width > 8192 || canvas_height > 8192 ||
        x + width > canvas_width || y + height > canvas_height) {
        return false;
    }

    PAddr output_address = config[0x64 / 4];
    if (output_buffer_count > 0) {
        output_address = LinearVirtualToPhysical(output_buffers[0]);
    }
    const u64 total_size = static_cast<u64>(canvas_width) * canvas_height * 2;
    const u64 output_offset = (static_cast<u64>(y) * canvas_width + x) * 2;
    if (output_address == 0 || total_size > std::numeric_limits<u32>::max() ||
        (output_buffer_size && total_size > output_buffer_size) ||
        !system.Memory().IsValidPhysicalAddress(output_address) ||
        !system.Memory().IsValidPhysicalAddress(output_address + static_cast<u32>(total_size - 1))) {
        return false;
    }

    u8* destination = system.Memory().GetPhysicalPointer(output_address);
    if (!destination || !decoder->Render(destination + output_offset, width, height, canvas_width,
                                         output_type)) {
        return false;
    }
    system.GPU().InvalidateRegion(output_address, static_cast<u32>(total_size));
    LOG_TRACE(Service, "Rendered {}x{} MVD frame with {} decoder", width, height,
              decoder->IsHardwareAccelerated() ? "MediaCodec" : "software");
    return true;
}
#endif

} // namespace Service::MVD
