package org.nx2a;

/** Uses the same language selection as the native pause menu. */
final class GameUi {
    static { System.loadLibrary("dmhost"); }
    private GameUi() {}
    private static native boolean isChinese();

    static String tr(String english, String chinese) {
        return isChinese() ? chinese : english;
    }
}
