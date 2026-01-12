{
    "targets": [
        {
            "target_name": "webdash_napi",
            "sources": [
                "src/webdash-napi.cpp",
                "../../src/webdash-core.cpp",
                "../../src/webdash-config.cpp",
                "../../src/webdash-config-task.cpp",
                "../../src/webdash-utils.cpp"
            ],
            "include_dirs": [
                "<!@(node -p \"require('node-addon-api').include\")",
                "../../include",
                "../../../external/json/include"
            ],
            "defines": [
                "NAPI_VERSION=8",
                "WEBDASH_PROJECT_NAME=\"wallpaper-engine-dashboard\""
            ],
            "cflags!": ["-fno-exceptions"],
            "cflags_cc!": ["-fno-exceptions"],
            "cflags_cc": ["-std=c++20", "-fexceptions"],
            "conditions": [
                ["OS=='linux'", {
                    "defines": ["_PLATFORM_LINUX"],
                    "libraries": ["-lboost_filesystem", "-lboost_system"]
                }],
                ["OS=='win'", {
                    "defines": ["_MSC_VER"],
                    "msvs_settings": {
                        "VCCLCompilerTool": {
                            "ExceptionHandling": 1,
                            "AdditionalOptions": ["/std:c++20"]
                        }
                    }
                }]
            ]
        }
    ]
}
