// Copyright 2016 Citra Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#pragma once

#include <array>
#include <memory>

#include "core/hle/service/service.h"

namespace Core {
class System;
}

namespace Service::MVD {

class MVD_STD final : public ServiceFramework<MVD_STD> {
public:
    explicit MVD_STD(Core::System& system);
    ~MVD_STD();

private:
    void Initialize(Kernel::HLERequestContext& ctx);
    void Shutdown(Kernel::HLERequestContext& ctx);
    void CalculateWorkBufSize(Kernel::HLERequestContext& ctx);
    void CalculateImageSize(Kernel::HLERequestContext& ctx);
    void ProcessNALUnit(Kernel::HLERequestContext& ctx);
    void ControlFrameRendering(Kernel::HLERequestContext& ctx);
    void GetStatus(Kernel::HLERequestContext& ctx);
    void GetStatusOther(Kernel::HLERequestContext& ctx);
    void GetConfig(Kernel::HLERequestContext& ctx);
    void SetConfig(Kernel::HLERequestContext& ctx);
    void SetOutputBuffer(Kernel::HLERequestContext& ctx);
    void OverrideOutputBuffers(Kernel::HLERequestContext& ctx);

#ifdef AZAHAR_MVD_FFMPEG
    class Decoder;
    bool RenderDecodedFrame();

    Core::System& system;
    std::unique_ptr<Decoder> decoder;
    std::array<u32, 0x11C / sizeof(u32)> config{};
    std::array<u32, 17 * 2> output_buffers{};
    u32 output_buffer_count{};
    u32 output_buffer_size{};
    u32 last_status{0x17000};
    bool initialized{};
#endif

    SERVICE_SERIALIZATION_SIMPLE
};

} // namespace Service::MVD

BOOST_CLASS_EXPORT_KEY(Service::MVD::MVD_STD)
SERVICE_CONSTRUCT(Service::MVD::MVD_STD)
