#!/usr/bin/env sh

OPTIMIZE="${GODOT_OPTIMIZE:-speed_trace}"
OS="$(uname -s)"
ARCH="$(uname -m)"
SCONS="$(command -v scons 2>/dev/null)"
HOME_DIR="${HOME:-${PWD}}"
TMP_DIR="${TMPDIR:-/tmp}"
VULKAN_SDK_ROOT="${VULKAN_SDK_ROOT:-${VULKAN_SDK:-}}"
MOLTENVK_ROOT="${MOLTENVK_ROOT:-${HOME_DIR}/MoltenVK/MoltenVK}"
MOLTENVK_STATIC_ROOT="${MOLTENVK_STATIC_ROOT:-${MOLTENVK_ROOT}/static/MoltenVK.xcframework}"

case "${ARCH}" in
    amd64|x86_64)
        ARCH="x86_64"
        ;;
    aarch64|arm64)
        ARCH="arm64"
        ;;
esac

if [ -z "${SCONS}" ]; then
    echo "scons not found in PATH" >&2
    exit 1
fi

case "${OS}" in
    Darwin)
        DEVELOPER_DIR="${DEVELOPER_DIR:-/Applications/Xcode.app/Contents/Developer}"
        TOOLCHAIN_BIN="${DEVELOPER_DIR}/Toolchains/XcodeDefault.xctoolchain/usr/bin"
        XCRUN="$(command -v xcrun 2>/dev/null)"

        if [ -z "${XCRUN}" ]; then
            echo "xcrun not found in PATH" >&2
            exit 1
        fi

        MOLTENVK_STATIC_ROOT="$(cd "${MOLTENVK_STATIC_ROOT}" 2>/dev/null && pwd)"
        MOLTENVK_STATIC_ARCHIVE="${MOLTENVK_STATIC_ROOT}/macos-arm64_x86_64/libMoltenVK.a"
        if [ ! -f "${MOLTENVK_STATIC_ARCHIVE}" ]; then
            echo "patched static MoltenVK universal archive not found: ${MOLTENVK_STATIC_ARCHIVE}" >&2
            echo "set MOLTENVK_STATIC_ROOT to the patched MoltenVK.xcframework" >&2
            exit 1
        fi
        MOLTENVK_STATIC_SHA256="$(shasum -a 256 "${MOLTENVK_STATIC_ARCHIVE}" | awk '{print $1}')"
        if [ -z "${MOLTENVK_STATIC_SHA256}" ]; then
            echo "failed to hash patched static MoltenVK archive: ${MOLTENVK_STATIC_ARCHIVE}" >&2
            exit 1
        fi
        echo "Using patched static MoltenVK archive: ${MOLTENVK_STATIC_ARCHIVE}"
        echo "Patched static MoltenVK SHA-256: ${MOLTENVK_STATIC_SHA256}"

        MACOS_SDK_PATH="$("${XCRUN}" --sdk macosx --show-sdk-path)"
        BUILD_PATH="${TOOLCHAIN_BIN}:${PATH}"

        if [ -z "${VULKAN_SDK_ROOT}" ] && [ -d "${HOME_DIR}/VulkanSDK" ]; then
            for SDK_CANDIDATE in "${HOME_DIR}"/VulkanSDK/*; do
                if [ -d "${SDK_CANDIDATE}/macOS/lib" ]; then
                    VULKAN_SDK_ROOT="${SDK_CANDIDATE}"
                fi
            done
        fi

        if [ -n "${VULKAN_SDK_ROOT}" ]; then
            if [ ! -d "${VULKAN_SDK_ROOT}" ]; then
                echo "Vulkan SDK root does not exist: ${VULKAN_SDK_ROOT}" >&2
                exit 1
            fi
            VULKAN_SDK_ROOT="$(cd "${VULKAN_SDK_ROOT}" && pwd)"
            VULKAN_LIB_DIR="${VULKAN_SDK_ROOT}/macOS/lib"
            VULKAN_ICD_PATH="${VULKAN_SDK_ROOT}/macOS/share/vulkan/icd.d/MoltenVK_icd.json"
            VULKAN_LOADER_PATH=""
            for VULKAN_CANDIDATE in "${VULKAN_LIB_DIR}"/libvulkan*.dylib; do
                case "${VULKAN_CANDIDATE}" in
                    *kosmickrisp*)
                        continue
                        ;;
                esac
                if [ -f "${VULKAN_CANDIDATE}" ]; then
                    VULKAN_LOADER_PATH="${VULKAN_CANDIDATE}"
                fi
            done
            if [ -z "${VULKAN_LOADER_PATH}" ]; then
                echo "Vulkan SDK loader not found under: ${VULKAN_LIB_DIR}" >&2
                exit 1
            fi
            MOLTENVK_DYLIB_PATH=""
            MOLTENVK_FORK_ICD_PATH=""
            if [ -d "${MOLTENVK_ROOT}" ]; then
                for MOLTENVK_CANDIDATE in \
                    "${MOLTENVK_ROOT}/dynamic/dylib/macOS/libMoltenVK.dylib" \
                    "${MOLTENVK_ROOT}/macOS/lib/libMoltenVK.dylib" \
                    "${MOLTENVK_ROOT}/libMoltenVK.dylib"; do
                    case "${MOLTENVK_CANDIDATE}" in
                        */dynamic/dylib/macOS/libMoltenVK.dylib)
                            MOLTENVK_CANDIDATE_ICD="${MOLTENVK_ROOT}/dynamic/dylib/macOS/MoltenVK_icd.json"
                            ;;
                        */macOS/lib/libMoltenVK.dylib)
                            MOLTENVK_CANDIDATE_ICD="${MOLTENVK_ROOT}/macOS/share/vulkan/icd.d/MoltenVK_icd.json"
                            ;;
                        *)
                            MOLTENVK_CANDIDATE_ICD="${MOLTENVK_ROOT}/MoltenVK_icd.json"
                            ;;
                    esac
                    if [ -f "${MOLTENVK_CANDIDATE}" ] && [ -f "${MOLTENVK_CANDIDATE_ICD}" ]; then
                        MOLTENVK_DYLIB_PATH="${MOLTENVK_CANDIDATE}"
                        MOLTENVK_FORK_ICD_PATH="${MOLTENVK_CANDIDATE_ICD}"
                        break
                    fi
                done
            fi

            VULKAN_RUNTIME_DIR="${PWD}/bin/vulkan_runtime"
            VULKAN_RUNTIME_LIB_DIR="${VULKAN_RUNTIME_DIR}/lib"
            VULKAN_RUNTIME_ICD_DIR="${VULKAN_RUNTIME_DIR}/share/vulkan/icd.d"
            VULKAN_RUNTIME_LAYER_DIR="${VULKAN_RUNTIME_DIR}/share/vulkan/explicit_layer.d"
            mkdir -p "${VULKAN_RUNTIME_LIB_DIR}" "${VULKAN_RUNTIME_ICD_DIR}" "${VULKAN_RUNTIME_LAYER_DIR}"
            for VULKAN_CANDIDATE in "${VULKAN_LIB_DIR}"/libvulkan*.dylib; do
                case "${VULKAN_CANDIDATE}" in
                    *kosmickrisp*)
                        continue
                        ;;
                esac
                if [ -f "${VULKAN_CANDIDATE}" ]; then
                    cp -f "${VULKAN_CANDIDATE}" "${VULKAN_RUNTIME_LIB_DIR}/$(basename "${VULKAN_CANDIDATE}")"
                fi
            done
            for VULKAN_CANDIDATE in "${VULKAN_LIB_DIR}"/libVkLayer*.dylib; do
                if [ -f "${VULKAN_CANDIDATE}" ]; then
                    cp -f "${VULKAN_CANDIDATE}" "${VULKAN_RUNTIME_LIB_DIR}/$(basename "${VULKAN_CANDIDATE}")"
                fi
            done
            for VULKAN_CANDIDATE in "${VULKAN_SDK_ROOT}"/macOS/share/vulkan/explicit_layer.d/*.json; do
                if [ -f "${VULKAN_CANDIDATE}" ]; then
                    cp -f "${VULKAN_CANDIDATE}" "${VULKAN_RUNTIME_LAYER_DIR}/$(basename "${VULKAN_CANDIDATE}")"
                fi
            done
            if [ -f "${VULKAN_RUNTIME_ICD_DIR}/libMoltenVK.dylib" ]; then
                rm -f "${VULKAN_RUNTIME_ICD_DIR}/libMoltenVK.dylib"
            fi
            if [ -n "${MOLTENVK_DYLIB_PATH}" ]; then
                cp -f "${MOLTENVK_DYLIB_PATH}" "${VULKAN_RUNTIME_LIB_DIR}/libMoltenVK.dylib"
                cp -f "${MOLTENVK_FORK_ICD_PATH}" "${VULKAN_RUNTIME_ICD_DIR}/MoltenVK_icd.json"
                sed -i '' 's|"./libMoltenVK.dylib"|"../../../lib/libMoltenVK.dylib"|' "${VULKAN_RUNTIME_ICD_DIR}/MoltenVK_icd.json"
                echo "Using patched MoltenVK: ${MOLTENVK_DYLIB_PATH}"
            else
                if [ ! -f "${VULKAN_LIB_DIR}/libMoltenVK.dylib" ] || [ ! -f "${VULKAN_ICD_PATH}" ]; then
                    echo "MoltenVK dylib/ICD not found in SDK and patched fork is unavailable" >&2
                    exit 1
                fi
                cp -f "${VULKAN_LIB_DIR}/libMoltenVK.dylib" "${VULKAN_RUNTIME_LIB_DIR}/libMoltenVK.dylib"
                cp -f "${VULKAN_ICD_PATH}" "${VULKAN_RUNTIME_ICD_DIR}/MoltenVK_icd.json"
                echo "Patched MoltenVK fork not found; using SDK MoltenVK"
            fi
            VULKAN_LAYER_PATH="${VULKAN_RUNTIME_LAYER_DIR}"
            echo "Using Vulkan SDK: ${VULKAN_SDK_ROOT}"
            echo "Using Vulkan loader: ${VULKAN_LOADER_PATH}"
            echo "Using Vulkan runtime stage: ${VULKAN_RUNTIME_DIR}"
        fi

        if [ -n "${VULKAN_SDK_ROOT}" ]; then
            exec env -i \
                DEVELOPER_DIR="${DEVELOPER_DIR}" \
                DYLD_LIBRARY_PATH="${VULKAN_LIB_DIR}" \
                HOME="${HOME_DIR}" \
                NO_COLOR=1 \
                PATH="${BUILD_PATH}" \
                TMPDIR="${TMP_DIR}" \
                VULKAN_SDK="${VULKAN_SDK_ROOT}" \
                VK_ICD_FILENAMES="${VULKAN_RUNTIME_ICD_DIR}/MoltenVK_icd.json" \
                VK_LAYER_PATH="${VULKAN_LAYER_PATH}" \
                "${SCONS}" platform=macos target=editor arch="${ARCH}" linker=lld \
                dev_build=yes optimize="${OPTIMIZE}" debug_symbols=yes angle=no \
                accesskit=no disable_xr=yes MACOS_SDK_PATH="${MACOS_SDK_PATH}" \
                vulkan_sdk_path="${MOLTENVK_STATIC_ROOT}" -j8 "$@"
        fi

        exec env -i \
            DEVELOPER_DIR="${DEVELOPER_DIR}" \
            HOME="${HOME_DIR}" \
            NO_COLOR=1 \
            PATH="${BUILD_PATH}" \
            TMPDIR="${TMP_DIR}" \
            "${SCONS}" platform=macos target=editor arch="${ARCH}" linker=lld \
            dev_build=yes optimize="${OPTIMIZE}" debug_symbols=yes angle=no \
            accesskit=no disable_xr=yes MACOS_SDK_PATH="${MACOS_SDK_PATH}" \
            vulkan_sdk_path="${MOLTENVK_STATIC_ROOT}" -j8 "$@"
        ;;
    Linux)
        exec env -i \
            HOME="${HOME_DIR}" \
            NO_COLOR=1 \
            PATH="${PATH}" \
            TMPDIR="${TMP_DIR}" \
            "${SCONS}" platform=linuxbsd target=editor arch="${ARCH}" linker=lld \
            dev_build=yes optimize="${OPTIMIZE}" debug_symbols=yes angle=no \
            accesskit=no disable_xr=yes -j8 "$@"
        ;;
    *)
        echo "unsupported host platform: ${OS}" >&2
        exit 1
        ;;
esac
