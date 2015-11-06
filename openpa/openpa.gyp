{
    "targets": [
        {
            "target_name": "openpa",
            "target_arch%": "x86",
            "type": "static_library",

            "sources": [
                "src/primitives/opa_emulated.h",
                "src/primitives/opa_nt_intrinics.h",
                "src/primitives/opa_gcc_intrinsics.h",
                "src/opa_config.h",
                "src/opa_primitives.h",
                "src/opa_primitives.c",
                "src/opa_util.h",
                "src/opa_queue.h",
                "src/opa_queue.c"
            ],
            "include_dirs": [
                "src/primitves",
                "src"
            ],
            "conditions": [
                ["OS=='win'", {
                    "sources": [
                        "src/primitives/opa_nt_intrinsics.h"
                    ],
                    "defines": [
                        "OPA_HAVE_NT_INTRINSICS=1",
                        "_opa_inline=__inline"
                    ],
                    "conditions": [
                        ["target_arch=='x64'", {
                            "VCLibrarianTool": {
                              "AdditionalOptions": [
                                "/MACHINE:X64",
                              ],
                            },
                        }, {
                            "VCLibrarianTool": {
                              "AdditionalOptions": [
                                "/MACHINE:x86",
                              ],
                            },
                        }],
                    ]
                }],
                ["OS=='mac' or OS=='linux'", {
                    "sources": [
                        "src/primitives/opa_gcc_intrinsics.h"
                    ],
                    "defines": [
                        "OPA_HAVE_GCC_INTRINSIC_ATOMICS=1",
                        "HAVE_STDDEF_H=1",
                        "HAVE_STDLIB_H=1",
                        "HAVE_UNISTD_H=1"
                    ]
                }],
                ["target_arch=='x64' or target_arch=='arm64'", {
                    "defines": [
                        "OPA_SIZEOF_VOID_P=8"
                    ]
                }],
                ["target_arch=='ia32' or target_arch=='armv7'", {
                    "defines": [
                        "OPA_SIZEOF_VOID_P=4"
                    ]
                }]
            ],
            "direct_dependent_settings": {
                "include_dirs": [
                    "src"
                ]
            }
        }
    ]
}
