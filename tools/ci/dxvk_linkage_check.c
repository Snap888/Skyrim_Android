/* CI-only ABI/linkage probe. This is not a replacement for generated Skyrim code. */
typedef void* (*SnapDxvkCreate9Fn)(unsigned int sdk_version);

extern void* Direct3DCreate9(unsigned int sdk_version);

__attribute__((visibility("default")))
SnapDxvkCreate9Fn snap_dxvk_linkage_probe(void) {
    return &Direct3DCreate9;
}
