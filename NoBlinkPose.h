// SPDX-License-Identifier: GPL-3.0-only
// Included in NoBlink.cpp's anonymous namespace after TryRead.

struct PoseSnapshot
{
    void* Model{};
    void* Descriptor{};
    uint16_t Bones{};
    bool Valid{};
    float Matrices[256][16]{};
};

bool ReadPoseBinding(void* model, void*& descriptor, uint16_t& bones, void*& matrices)
{
    void* handle = nullptr;
    // Existing du1-617977e089d42a62: skeleton descriptor and aligned matrix workset.
    return TryRead(model, 0x0C, handle) && TryRead(handle, 0, descriptor) &&
        TryRead(descriptor, 0x32, bones) && bones != 0 && bones <= 256 &&
        TryRead(model, 0x14, matrices) && matrices != nullptr;
}

void CapturePose(void* model, PoseSnapshot& pose)
{
    pose.Valid = false;
    pose.Model = model;
    void* matrices = nullptr;
    if (!ReadPoseBinding(model, pose.Descriptor, pose.Bones, matrices)) return;
    for (uint32_t i = 0; i < pose.Bones; ++i)
    {
        if (!TryRead(matrices, i * 64, pose.Matrices[i])) return;
        for (const auto value : pose.Matrices[i]) if (!std::isfinite(value)) return;
    }
    pose.Valid = true;
}

bool RestorePose(void* model, const PoseSnapshot& pose, uint32_t& changed)
{
    changed = 0;
    void* descriptor = nullptr;
    void* matrices = nullptr;
    uint16_t bones = 0;
    if (!pose.Valid || model != pose.Model || !ReadPoseBinding(model, descriptor, bones, matrices) ||
        descriptor != pose.Descriptor || bones != pose.Bones) return false;
    const size_t bytes = static_cast<size_t>(bones) * 64;
    if (reinterpret_cast<uintptr_t>(matrices) > UINTPTR_MAX - bytes ||
        ::IsBadReadPtr(matrices, bytes) || ::IsBadWritePtr(matrices, bytes)) return false;
    for (uint32_t i = 0; i < bones; ++i)
        if (std::memcmp(static_cast<const uint8_t*>(matrices) + i * 64, pose.Matrices[i], 64) != 0)
            ++changed;
    // Keep the existing animation pose until the next native animation update. Preserve the new
    // resource binding, mask, allocations and ownership; never transplant a different skeleton.
    std::memcpy(matrices, pose.Matrices, bytes);
    return true;
}
