// FUSE UI Android host activity (G4). A NativeActivity whose native side is fuse::app::AndroidPlatform, plus:
//  * FuseInputView: an invisible, focusable overlay View that owns the InputConnection, so the soft keyboard / IME
//    delivers committed and composing text (nativeText) instead of synthetic key events;
//  * an AccessibilityNodeProvider on that view exposing UiContext::snapshotA11y (nativeA11y*) to TalkBack.
package org.fuse.ui;

import android.app.NativeActivity;
import android.content.Context;
import android.graphics.Rect;
import android.os.Bundle;
import android.text.InputType;
import android.view.View;
import android.view.ViewGroup;
import android.view.accessibility.AccessibilityNodeInfo;
import android.view.accessibility.AccessibilityNodeProvider;
import android.view.inputmethod.BaseInputConnection;
import android.view.inputmethod.EditorInfo;
import android.view.inputmethod.InputConnection;
import android.view.inputmethod.InputMethodManager;

public class FuseActivity extends NativeActivity {
    static native void nativeText(boolean commit, String text, int caret);
    static native int nativeA11yCount();
    static native float[] nativeA11yInfo(int index);
    static native String nativeA11yString(int index, int which); // 0 = name, 1 = class name
    static native void nativeA11yClick(int index);

    private FuseInputView input;

    @Override protected void onCreate(Bundle saved) {
        // NativeActivity dlopens android.app.lib_name itself, outside this class loader's JNI namespace, so the
        // native* methods below would not resolve. Load the same library through System.loadLibrary first.
        try {
            android.content.pm.ActivityInfo ai = getPackageManager().getActivityInfo(getIntent().getComponent(),
                    android.content.pm.PackageManager.GET_META_DATA);
            String lib = ai.metaData != null ? ai.metaData.getString("android.app.lib_name") : null;
            if (lib != null) System.loadLibrary(lib);
        } catch (android.content.pm.PackageManager.NameNotFoundException e) {
            throw new RuntimeException(e);
        }
        super.onCreate(saved);
        input = new FuseInputView(this);
        addContentView(input, new ViewGroup.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.MATCH_PARENT));
    }

    /** Called from the native glue thread (AndroidPlatform::Impl::setIme). */
    public void setImeVisible(final boolean on) {
        runOnUiThread(new Runnable() { @Override public void run() {
            InputMethodManager imm = (InputMethodManager) getSystemService(Context.INPUT_METHOD_SERVICE);
            if (on) {
                input.requestFocus();
                imm.showSoftInput(input, 0);
            } else {
                imm.hideSoftInputFromWindow(input.getWindowToken(), 0);
            }
        }});
    }

    static final class FuseInputView extends View {
        FuseInputView(Context c) {
            super(c);
            setFocusable(true);
            setFocusableInTouchMode(true);
            setImportantForAccessibility(IMPORTANT_FOR_ACCESSIBILITY_YES);
        }
        @Override public boolean onCheckIsTextEditor() { return true; }
        @Override public InputConnection onCreateInputConnection(EditorInfo info) {
            info.inputType = InputType.TYPE_CLASS_TEXT;
            info.imeOptions = EditorInfo.IME_FLAG_NO_FULLSCREEN | EditorInfo.IME_ACTION_DONE;
            return new BaseInputConnection(this, false) {
                @Override public boolean commitText(CharSequence t, int pos) { nativeText(true, t.toString(), 0); return true; }
                @Override public boolean setComposingText(CharSequence t, int pos) {
                    nativeText(false, t.toString(), t.length()); return true;
                }
                @Override public boolean finishComposingText() { return true; }
            };
        }

        private final AccessibilityNodeProvider provider = new AccessibilityNodeProvider() {
            @Override public AccessibilityNodeInfo createAccessibilityNodeInfo(int id) {
                final int n = nativeA11yCount();
                if (id == View.NO_ID) {
                    AccessibilityNodeInfo root = AccessibilityNodeInfo.obtain(FuseInputView.this);
                    onInitializeAccessibilityNodeInfo(root);
                    for (int i = 0; i < n; ++i) {
                        float[] f = nativeA11yInfo(i);
                        if (f != null && f[0] < 0) root.addChild(FuseInputView.this, i);
                    }
                    return root;
                }
                float[] f = nativeA11yInfo(id);
                if (f == null) return null;
                AccessibilityNodeInfo info = AccessibilityNodeInfo.obtain(FuseInputView.this, id);
                if (f[0] < 0) info.setParent(FuseInputView.this); else info.setParent(FuseInputView.this, (int) f[0]);
                for (int i = 0; i < n; ++i) {
                    float[] c = nativeA11yInfo(i);
                    if (c != null && (int) c[0] == id) info.addChild(FuseInputView.this, i);
                }
                info.setClassName(nativeA11yString(id, 1));
                info.setText(nativeA11yString(id, 0));
                info.setPackageName(getContext().getPackageName());
                info.setFocusable(f[1] != 0);
                info.setFocused(f[2] != 0);
                info.setCheckable(false);
                info.setChecked(f[3] != 0);
                info.setEnabled(f[4] == 0);
                info.setVisibleToUser(true);
                final Rect r = new Rect((int) f[5], (int) f[6], (int) (f[5] + f[7]), (int) (f[6] + f[8]));
                info.setBoundsInParent(r); // deprecated; screen bounds below are what services use
                // FUSE bounds are in the native window's (surface) coordinates, not this overlay view's: offset by the
                // window's screen origin only (screen location minus in-window location).
                int[] scr = new int[2], win = new int[2];
                getLocationOnScreen(scr);
                getLocationInWindow(win);
                r.offset(scr[0] - win[0], scr[1] - win[1]);
                info.setBoundsInScreen(r);
                if (f[11] > f[10]) info.setRangeInfo(AccessibilityNodeInfo.RangeInfo.obtain(
                        AccessibilityNodeInfo.RangeInfo.RANGE_TYPE_FLOAT, f[10], f[11], f[9]));
                info.setClickable(f[1] != 0);
                if (f[1] != 0) info.addAction(AccessibilityNodeInfo.AccessibilityAction.ACTION_CLICK);
                return info;
            }
            @Override public boolean performAction(int id, int action, Bundle args) {
                if (action == AccessibilityNodeInfo.ACTION_CLICK && id >= 0) { nativeA11yClick(id); return true; }
                return false;
            }
        };
        @Override public AccessibilityNodeProvider getAccessibilityNodeProvider() { return provider; }
    }
}
