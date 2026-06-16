// pre.js — runs before main(). Copies URL query params into Emscripten's ENV
// so C++ getenv() calls work identically on web and desktop.
// Usage: index.html?CV_TEST_FRAMES=120&CV_FIXED_TIME=2.0&CV_SCREENSHOT=1
var Module = typeof Module !== 'undefined' ? Module : {};
Module.preRun = Module.preRun || [];
Module.preRun.push(function () {
    new URLSearchParams(location.search).forEach(function (v, k) {
        ENV[k] = v;
    });
});
