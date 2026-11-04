rockspec_format = "3.0"
package = "awesomewm-screenlock-plugin"
version = "0.1.0-1"
source = {
    url = "https://github.com/macunha1/awesomewm-screenlock-plugin/archive/refs/tags/v0.1.0.tar.gz",
    file = "awesomewm-screenlock-plugin-0.1.0.tar.gz",
    dir = "awesomewm-screenlock-plugin-0.1.0",
}
description = {
    summary = "AwesomeWM screenlock with a Lua API and native X11/PAM helper",
    homepage = "https://github.com/macunha1/awesomewm-screenlock-plugin",
    license = "MIT",
}
supported_platforms = { "linux" }
dependencies = { "lua >= 5.1, < 5.3" }
build = {
    type = "command",
    build_command = "sh build/luarocks.sh $(LUA_VERSION)",
    install = {
        lua = {
            awesomewm_screenlock = "lua/awesomewm_screenlock/init.lua",
            ["plugins.screenlock"] = "lua/plugins/screenlock.lua",
        },
        bin = { "build/luarocks/awesomewm-screenlock-helper" },
    },
    copy_directories = {},
}
