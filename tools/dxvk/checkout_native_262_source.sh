#!/usr/bin/env bash
set -euo pipefail

# Reproducibly materialize the reviewed upstream DXVK Native 2.6.2 source.
# This is a source audit/preparation step, not an Android port or binary build.
readonly repo="https://github.com/doitsujin/dxvk.git"
readonly tag="v2.6.2"
readonly expected_commit="9d6f54a1ade20d1d27dd421024717a636f3d8c68"
dest="${1:-${TMPDIR:-/tmp}/dxvk-native-2.6.2}"

if [[ -e "$dest" ]]; then
  echo "::error::Destination already exists: $dest" >&2
  exit 2
fi

git init "$dest"
git -C "$dest" remote add origin "$repo"
git -C "$dest" fetch --depth=1 origin "$tag"
actual_commit="$(git -C "$dest" rev-parse FETCH_HEAD^{commit})"
if [[ "$actual_commit" != "$expected_commit" ]]; then
  echo "::error::Pinned tag resolved to $actual_commit, expected $expected_commit" >&2
  exit 3
fi
git -C "$dest" checkout --detach "$expected_commit"

required_files=(
  src/d3d9/d3d9_swapchain.cpp
  src/d3d9/d3d9_swapchain.h
  src/d3d9/meson.build
  src/dxvk/dxvk_presenter.cpp
  src/dxvk/dxvk_presenter.h
  src/wsi/meson.build
  src/wsi/sdl2/wsi_platform_sdl2.cpp
  meson.build
)
for path in "${required_files[@]}"; do
  test -s "$dest/$path" || {
    echo "::error::Pinned source is missing required file: $path" >&2
    exit 4
  }
done

# Apply the pinned Android WSI/Presenter overlay in the disposable checkout.
# This is a source transformation check only; it does not imply a successful
# Android compile or a packaged runtime pair.
python3 "$project_root/tools/dxvk/apply_native_262_android_port.py" \
  "$dest" --project-root "$project_root"

# Fail if the source-level presenter contract changes unexpectedly. These
# anchors are reviewed integration points, not a substitute for compilation.
grep -q 'acquireNextImage' "$dest/src/d3d9/d3d9_swapchain.cpp"
grep -q 'EmitCs' "$dest/src/d3d9/d3d9_swapchain.cpp"
grep -q 'synchronizeWsi' "$dest/src/d3d9/d3d9_swapchain.cpp"
grep -q 'presentImage' "$dest/src/dxvk/dxvk_presenter.cpp"


# Verify that the Android integration seam being ported still exists in this
# checkout. These checks prevent the source map from drifting away from code.
readonly project_root="$(git rev-parse --show-toplevel)"
native_surface_cpp="$project_root/android/app/jni/dxvk/src/wsi/sdl2/wsi_native_surface.cpp"
native_surface_wsi="$project_root/android/app/jni/dxvk/src/wsi/sdl2/wsi_presenter_sdl2.cpp"
swapchain_cpp="$project_root/android/app/jni/dxvk/src/d3d9/d3d9_swapchain.cpp"
cmake_file="$project_root/android/app/jni/dxvk/CMakeLists.txt"

for path in "$native_surface_cpp" "$native_surface_wsi" "$swapchain_cpp" "$cmake_file"; do
  test -s "$path" || {
    echo "::error::Android integration seam is missing: $path" >&2
    exit 5
  }
done

grep -q 'dxvk_native_surface' "$native_surface_cpp"
grep -q 'ANativeWindow_acquire' "$native_surface_cpp"
grep -q 'ANativeWindow_release' "$native_surface_cpp"
grep -q 'releaseNativeSurface' "$native_surface_wsi"

# Enforce active-registration and temporary-lease ownership, not just API presence.
grep -q 'acquireWindow(window);' "$native_surface_cpp"
grep -q 'previousWindow = g_surface.window;' "$native_surface_cpp"
grep -q 'releaseWindow(previousWindow);' "$native_surface_cpp"
grep -q 'acquireWindow(pOut->window);' "$native_surface_cpp"
grep -q 'releaseWindow(pSurface->window);' "$native_surface_cpp"
grep -q 'vkCreateAndroidSurfaceKHR' "$native_surface_wsi"
grep -q 'vkCreateAndroidSurfaceKHR' "$native_surface_wsi"
grep -q 'SnapFrameCopyHook' "$swapchain_cpp"
grep -q 'SnapFramePresentResultHook' "$swapchain_cpp"
grep -q 'wsi_native_surface.cpp' "$cmake_file"

echo "DXVK_NATIVE_SOURCE_AUDIT_OK tag=$tag commit=$actual_commit"
echo "Android port status: NOT_PORTED"
echo "Build status: SOURCE_ONLY"
