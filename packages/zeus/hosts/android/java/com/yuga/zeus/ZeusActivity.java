package com.yuga.zeus;

import android.app.Activity;
import android.os.Bundle;
import android.view.Window;
import android.view.WindowManager;

/** Host activity. Zeus paints the full window; no Android widgets. */
public class ZeusActivity extends Activity {
    private ZeusView view;

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        requestWindowFeature(Window.FEATURE_NO_TITLE);
        getWindow().addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
        view = new ZeusView(this);
        setContentView(view);
    }

    @Override
    protected void onActivityResult(int requestCode, int resultCode, android.content.Intent data) {
        super.onActivityResult(requestCode, resultCode, data);
        if (requestCode == ZeusView.PICK_IMAGE && resultCode == RESULT_OK && data != null)
            view.pickedUri(data.getData());
    }
}
