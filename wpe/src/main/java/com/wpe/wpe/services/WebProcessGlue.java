package com.wpe.wpe.services;

public class WebProcessGlue {

    static {
        System.loadLibrary("WPEBackend-default");
        System.loadLibrary("WPEWebProcessGlue");
    }

    public static native void initializeXdg(String xdgCachePath);
    public static native void initializeFontconfig(String fontconfigPath);
    public static native void initializeGioExtraModulesPath(String extraModulesPath);
    public static native void initializeMain(int fd1, int fd2);
}
