include_guard(GLOBAL)

set(WINBUILD_DAVS2_COMMIT
    21d64c8f8e36af71fc7a488cd6f789c86cdd1200)
set(WINBUILD_UAVS3D_COMMIT
    0e20d2c291853f196c68922a264bcd8471d75b68)
set(WINBUILD_AVS_PATCH_COMMIT
    6788d317a3a67c44f799d02c4ff83f95d6b10165)

function(verify_avs_import relative_path expected_sha256)
    set(path "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/../${relative_path}")
    if(NOT EXISTS "${path}")
        message(FATAL_ERROR "Required AVS import is missing: ${path}")
    endif()
    file(SHA256 "${path}" actual_sha256)
    if(NOT actual_sha256 STREQUAL expected_sha256)
        message(FATAL_ERROR
            "AVS import hash mismatch for ${relative_path}: "
            "expected ${expected_sha256}, got ${actual_sha256}")
    endif()
endfunction()

verify_avs_import(
    packages/davs2-0001-enable-10bit-build-and-propagate-frame-packet-position.patch
    2e83a0f00092c58a4d5a3350ca6f48b1f411dbe088b9dd5c8812dcd2e5986f65)
verify_avs_import(
    packages/davs2-0002-enable-arm64-neon-detect-and-keep-vectorization.patch
    6d0c31f67bdefaa44f5bd2b5b59cd2ef6b97cd05adde9fe0e70f0be5bfe28129)
verify_avs_import(
    packages/davs2-0003-add-aarch64-neon-primitives-for-copy-add-avg.patch
    9e063173a7c70041c8fa31c00afa03d126e2aa8f25dcd025d428dccaaed30054)
verify_avs_import(
    packages/davs2-0004-add-aarch64-neon-mc-interpolation.patch
    ba4ce478ef69efe635f8b1582e4f69b6ed60b50ab334e8625e54f720d4b9a84d)
verify_avs_import(
    packages/davs2-0005-add-aarch64-neon-mc-ext-primitives.patch
    51d7c75911fe5c76cb3d6355c154b35318d2a409d9a7a58b3caf626e2e64187c)
verify_avs_import(
    packages/davs2-0006-add-aarch64-neon-deblock-luma.patch
    176575848ab314cc8787edd573c80cb56f6163a77b264bb2b2a2993a2c2e337a)
verify_avs_import(
    packages/davs2-0007-add-aarch64-neon-deblock-chroma.patch
    5363aa7714b23ee697ae5c275d8277b95f5365b966c26b0f4e8b4da021543571)
verify_avs_import(
    packages/davs2-0008-add-aarch64-neon-intra-basic-10bit.patch
    c3ab52931da7b5110579c933da0ca2045cea21a0260a8316168442525cef86fe)
verify_avs_import(
    packages/davs2-0009-add-aarch64-neon-intra-bilinear-10bit.patch
    e83616b399e164252255887290948480a2238095280597bdcc2bf5ad899fc28a)
verify_avs_import(
    packages/davs2-0010-export-sequence-display-color-description.patch
    64aa309b9217269c83ccd7183aad3b7980da76515a9700bbce9499cb85d58460)
verify_avs_import(
    packages/davs2-0012-use-standard-setjmp-on-windows-arm64.patch
    ce90d09171067c5d84e31cf070c9bee7634d48d4a8cef2b26dd70801779085c5)
verify_avs_import(
    packages/uavs3d-0001-use-portable-c-functions-on-windows-arm64.patch
    06e999884317732470a1598b9a875c98aafe202f2220ea7dce3ba25f7365d053)
verify_avs_import(
    notices/AVS-THIRD-PARTY-NOTICES.txt
    21bfffd34ee6644dd7acbeffc65a68afd449e3ec4427f1a67afe46c0fb38517d)
verify_avs_import(
    notices/GPL-2.0.txt
    edaef632cbb643e4e7a221717a6c441a4c1a7c918e6e4d56debc3d8739b233f6)
verify_avs_import(
    notices/GPL-3.0.txt
    8ceb4b9ee5adedde47b31e975c1d90c73ad27b6b165a1dcd80c7c545eb65b903)
verify_avs_import(
    notices/UAVS3D-BSD-3-Clause.txt
    5a8dcb7da222df8a81b6e334000f859248196335e2d28e1db9f3c552827d7cdf)
