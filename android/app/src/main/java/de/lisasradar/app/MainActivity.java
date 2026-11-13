package de.lisasradar.app;

import android.Manifest;
import android.app.Activity;
import android.app.AlertDialog;
import android.content.Context;
import android.content.Intent;
import android.content.SharedPreferences;
import android.content.pm.PackageManager;
import android.graphics.Color;
import android.graphics.Typeface;
import android.graphics.drawable.GradientDrawable;
import android.location.Location;
import android.location.LocationListener;
import android.location.LocationManager;
import android.net.Uri;
import android.net.nsd.NsdManager;
import android.net.nsd.NsdServiceInfo;
import android.net.wifi.WifiManager;
import android.os.Build;
import android.os.Bundle;
import android.os.CancellationSignal;
import android.os.Handler;
import android.os.Looper;
import android.provider.Settings;
import android.text.InputType;
import android.view.Gravity;
import android.view.View;
import android.view.ViewGroup;
import android.view.WindowInsets;
import android.webkit.ValueCallback;
import android.webkit.WebChromeClient;
import android.webkit.WebResourceError;
import android.webkit.WebResourceRequest;
import android.webkit.WebSettings;
import android.webkit.WebView;
import android.webkit.WebViewClient;
import android.widget.Button;
import android.widget.EditText;
import android.widget.FrameLayout;
import android.widget.LinearLayout;
import android.widget.ProgressBar;
import android.widget.TextView;
import android.widget.Toast;

import java.io.ByteArrayOutputStream;
import java.io.InputStream;
import java.io.OutputStream;
import java.net.HttpURLConnection;
import java.net.InetAddress;
import java.net.URL;
import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
import java.util.List;
import java.util.Locale;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;
import java.util.concurrent.atomic.AtomicBoolean;

/**
 * Lisas Radar: findet das Flug- & Wetterradar im WLAN, zeigt seine Einstellungsseite an und
 * schickt auf Knopfdruck den Standort des Handys als neuen Radar-Mittelpunkt.
 *
 * Bewusst ohne Bibliotheken (nur das Android-Framework), die Oberfläche wird im Code gebaut.
 * Suche: 1. die zuletzt benutzte Adresse, 2. lisasradar.local, 3. mDNS/NSD ("_http._tcp",
 * Gerätename lisasradar / capsuleradar). Erkannt wird das Radar an GET /api/info.
 * Standort: POST /loc (lat, lon) mit Header "X-Radar: 1" -- wie die Live-Schalter der Webseite.
 */
public class MainActivity extends Activity {
    private static final String PREFS = "radar";
    private static final String KEY_BASE = "base";
    private static final String DEFAULT_HOST = "http://lisasradar.local";
    private static final int GREEN = 0xFF1DFF86, BG = 0xFF06100A, PANEL = 0xFF0C1F14, DIM = 0xFF7FA08C;
    private static final int REQ_LOCATION = 1, REQ_FILE = 2;
    private static final long DISCOVERY_MS = 9000, FIX_TIMEOUT_MS = 25000;

    private final Handler ui = new Handler(Looper.getMainLooper());
    private final ExecutorService io = Executors.newCachedThreadPool();
    private SharedPreferences prefs;

    private WebView web;
    private LinearLayout overlay;          // "Suche ...", Fehler, Buttons
    private TextView overlayText, titleSub;
    private ProgressBar overlaySpin;
    private LinearLayout overlayButtons;
    private Button locButton;

    private String base;                    // e.g. http://192.168.178.42 -- null until found
    private int searchGen = 0;              // a new search cancels the old one
    private NsdManager nsd;
    private NsdManager.DiscoveryListener discovery;
    private WifiManager.MulticastLock mcLock;
    private ValueCallback<Uri[]> fileCallback;
    private AlertDialog busyDialog;
    private CancellationSignal fixCancel;
    private LocationListener legacyListener;

    // ------------------------------------------------------------------ lifecycle
    @Override
    protected void onCreate(Bundle state) {
        super.onCreate(state);
        prefs = getSharedPreferences(PREFS, MODE_PRIVATE);
        buildUi();
        startSearch();
    }

    @Override
    protected void onDestroy() {
        stopDiscovery();
        cancelFix();
        io.shutdownNow();
        if (web != null) web.destroy();
        super.onDestroy();
    }

    @Override
    public void onBackPressed() {
        if (web != null && web.getVisibility() == View.VISIBLE && web.canGoBack()) web.goBack();
        else super.onBackPressed();
    }

    // ------------------------------------------------------------------ UI
    private int dp(float v) { return Math.round(v * getResources().getDisplayMetrics().density); }

    private Button makeButton(String text, boolean primary) {
        Button b = new Button(this);
        b.setText(text);
        b.setAllCaps(false);
        b.setTextSize(16);
        b.setTextColor(primary ? Color.BLACK : GREEN);
        GradientDrawable d = new GradientDrawable();
        d.setCornerRadius(dp(14));
        if (primary) d.setColor(GREEN);
        else { d.setColor(PANEL); d.setStroke(dp(1), GREEN); }
        b.setBackground(d);
        b.setPadding(dp(16), dp(12), dp(16), dp(12));
        return b;
    }

    private void buildUi() {
        LinearLayout root = new LinearLayout(this);
        root.setOrientation(LinearLayout.VERTICAL);
        root.setBackgroundColor(BG);

        // title bar: name + address (tap: change address) + reload
        LinearLayout bar = new LinearLayout(this);
        bar.setOrientation(LinearLayout.HORIZONTAL);
        bar.setGravity(Gravity.CENTER_VERTICAL);
        bar.setPadding(dp(16), dp(10), dp(8), dp(10));
        LinearLayout titles = new LinearLayout(this);
        titles.setOrientation(LinearLayout.VERTICAL);
        TextView title = new TextView(this);
        title.setText(R.string.app_name);
        title.setTextColor(GREEN);
        title.setTextSize(20);
        title.setTypeface(Typeface.DEFAULT_BOLD);
        titleSub = new TextView(this);
        titleSub.setTextColor(DIM);
        titleSub.setTextSize(12);
        titleSub.setText("Suche ...");
        titles.addView(title);
        titles.addView(titleSub);
        titles.setOnClickListener(v -> askAddress());
        bar.addView(titles, new LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1));
        Button reload = new Button(this);
        reload.setText("↻");                       // ↻
        reload.setTextSize(22);
        reload.setTextColor(GREEN);
        reload.setBackgroundColor(Color.TRANSPARENT);
        reload.setOnClickListener(v -> {
            if (base != null && web.getVisibility() == View.VISIBLE) web.reload();
            else startSearch();
        });
        bar.addView(reload, new LinearLayout.LayoutParams(dp(52), dp(48)));
        root.addView(bar);

        // content: the radar's web page, with the search/error overlay on top
        FrameLayout content = new FrameLayout(this);
        web = new WebView(this);
        web.setBackgroundColor(BG);
        web.setVisibility(View.INVISIBLE);
        setupWebView();
        content.addView(web, new FrameLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.MATCH_PARENT));

        overlay = new LinearLayout(this);
        overlay.setOrientation(LinearLayout.VERTICAL);
        overlay.setGravity(Gravity.CENTER);
        overlay.setPadding(dp(28), dp(28), dp(28), dp(28));
        overlay.setBackgroundColor(BG);
        overlaySpin = new ProgressBar(this);
        overlay.addView(overlaySpin, new LinearLayout.LayoutParams(dp(48), dp(48)));
        overlayText = new TextView(this);
        overlayText.setTextColor(Color.WHITE);
        overlayText.setTextSize(16);
        overlayText.setGravity(Gravity.CENTER);
        overlayText.setPadding(0, dp(16), 0, dp(16));
        overlay.addView(overlayText);
        overlayButtons = new LinearLayout(this);
        overlayButtons.setOrientation(LinearLayout.VERTICAL);
        overlay.addView(overlayButtons);
        content.addView(overlay, new FrameLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.MATCH_PARENT));
        root.addView(content, new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, 0, 1));

        // the big button
        locButton = makeButton("📍  Meinen Standort ans Radar senden", true);   // 📍
        locButton.setEnabled(false);
        locButton.setAlpha(0.4f);
        locButton.setOnClickListener(v -> onLocationButton());
        LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT);
        lp.setMargins(dp(14), dp(10), dp(14), dp(12));
        root.addView(locButton, lp);

        // Android 15 draws edge-to-edge: keep the bars clear of the status/navigation bar
        root.setOnApplyWindowInsetsListener((v, insets) -> {
            int top, bottom, left, right;
            if (Build.VERSION.SDK_INT >= 30) {
                android.graphics.Insets i = insets.getInsets(WindowInsets.Type.systemBars() | WindowInsets.Type.ime());
                top = i.top; bottom = i.bottom; left = i.left; right = i.right;
            } else {
                top = insets.getSystemWindowInsetTop(); bottom = insets.getSystemWindowInsetBottom();
                left = insets.getSystemWindowInsetLeft(); right = insets.getSystemWindowInsetRight();
            }
            v.setPadding(left, top, right, bottom);
            return insets;
        });
        setContentView(root);
    }

    private void showOverlay(boolean spinning, String text, Button... buttons) {
        overlay.setVisibility(View.VISIBLE);
        overlaySpin.setVisibility(spinning ? View.VISIBLE : View.GONE);
        overlayText.setText(text);
        overlayButtons.removeAllViews();
        for (Button b : buttons) {
            LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(dp(260), ViewGroup.LayoutParams.WRAP_CONTENT);
            lp.setMargins(0, dp(6), 0, dp(6));
            overlayButtons.addView(b, lp);
        }
    }

    private void setLocEnabled(boolean on) {
        locButton.setEnabled(on);
        locButton.setAlpha(on ? 1f : 0.4f);
    }

    private void toast(String s) { Toast.makeText(this, s, Toast.LENGTH_LONG).show(); }

    // ------------------------------------------------------------------ WebView
    private void setupWebView() {
        WebSettings s = web.getSettings();
        s.setJavaScriptEnabled(true);           // the radar page's switches and sliders are JS
        s.setDomStorageEnabled(true);
        s.setAllowFileAccess(false);
        s.setLoadWithOverviewMode(true);
        s.setUseWideViewPort(true);
        web.setWebViewClient(new WebViewClient() {
            @Override
            public boolean shouldOverrideUrlLoading(WebView view, WebResourceRequest req) {
                Uri u = req.getUrl();
                Uri b = base != null ? Uri.parse(base) : null;
                if (b != null && u.getHost() != null && u.getHost().equalsIgnoreCase(b.getHost())) return false;
                try { startActivity(new Intent(Intent.ACTION_VIEW, u)); } catch (Exception ignored) { }
                return true;                    // links elsewhere (OpenStreetMap, GitHub ...) open in the browser
            }

            @Override
            public void onReceivedError(WebView view, WebResourceRequest req, WebResourceError err) {
                if (!req.isForMainFrame()) return;
                web.setVisibility(View.INVISIBLE);
                setLocEnabled(false);
                Button retry = makeButton("Erneut suchen", true);
                retry.setOnClickListener(v -> startSearch());
                Button addr = makeButton("Adresse eingeben", false);
                addr.setOnClickListener(v -> askAddress());
                showOverlay(false, "Das Radar antwortet gerade nicht.\n\nIst es eingeschaltet und im selben WLAN wie das Handy?", retry, addr);
            }
        });
        web.setWebChromeClient(new WebChromeClient() {
            @Override
            public boolean onShowFileChooser(WebView view, ValueCallback<Uri[]> cb, FileChooserParams params) {
                // photo upload (and the firmware update page) use <input type=file>
                if (fileCallback != null) fileCallback.onReceiveValue(null);
                fileCallback = cb;
                try {
                    startActivityForResult(params.createIntent(), REQ_FILE);
                } catch (Exception e) {
                    fileCallback = null;
                    return false;
                }
                return true;
            }
        });
    }

    @Override
    protected void onActivityResult(int req, int res, Intent data) {
        super.onActivityResult(req, res, data);
        if (req != REQ_FILE || fileCallback == null) return;
        fileCallback.onReceiveValue(WebChromeClient.FileChooserParams.parseResult(res, data));
        fileCallback = null;
    }

    private void openRadar(String found, String version) {
        base = found;
        prefs.edit().putString(KEY_BASE, found).apply();
        titleSub.setText(Uri.parse(found).getHost() + (version != null ? "  ·  Firmware " + version : ""));
        overlay.setVisibility(View.GONE);
        web.setVisibility(View.VISIBLE);
        web.loadUrl(found + "/");
        setLocEnabled(true);
    }

    // ------------------------------------------------------------------ finding the radar
    /** Result of a probe: null = not a radar, else the firmware version ("" if unknown). */
    private static String probe(String b) {
        try {
            HttpURLConnection c = (HttpURLConnection) new URL(b + "/api/info").openConnection();
            c.setConnectTimeout(2500);
            c.setReadTimeout(3000);
            int code = c.getResponseCode();
            if (code == 200) {
                String body = readAll(c.getInputStream());
                if (body.contains("\"capsuleradar\"")) {
                    int i = body.indexOf("\"version\":\"");
                    if (i < 0) return "";
                    int j = body.indexOf('"', i + 11);
                    return j > i ? body.substring(i + 11, j) : "";
                }
                return null;
            }
            c.disconnect();
            // firmware older than the app (no /api/info yet): recognise the config page itself
            c = (HttpURLConnection) new URL(b + "/").openConnection();
            c.setConnectTimeout(2500);
            c.setReadTimeout(4000);
            if (c.getResponseCode() == 200) {
                String body = readAll(c.getInputStream());
                if (body.contains("X-Radar")) return "";
            }
        } catch (Exception ignored) { }
        return null;
    }

    private static String readAll(InputStream in) throws java.io.IOException {
        ByteArrayOutputStream out = new ByteArrayOutputStream();
        byte[] buf = new byte[4096];
        int n;
        while ((n = in.read(buf)) > 0 && out.size() < 200_000) out.write(buf, 0, n);
        in.close();
        return out.toString("UTF-8");
    }

    private void startSearch() {
        final int gen = ++searchGen;
        stopDiscovery();
        base = null;
        setLocEnabled(false);
        web.setVisibility(View.INVISIBLE);
        titleSub.setText("Suche ...");
        showOverlay(true, "Suche dein Radar im WLAN ...");
        final List<String> tries = new ArrayList<>();
        String saved = prefs.getString(KEY_BASE, null);
        if (saved != null) tries.add(saved);
        if (!tries.contains(DEFAULT_HOST)) tries.add(DEFAULT_HOST);
        io.execute(() -> {
            for (String b : tries) {
                if (gen != searchGen) return;
                String v = probe(b);
                if (v != null) {
                    // keep a numeric address: .local names are not resolved on every phone
                    String keep = b;
                    try {
                        Uri u = Uri.parse(b);
                        String ip = InetAddress.getByName(u.getHost()).getHostAddress();
                        if (ip != null && !ip.contains(":")) keep = "http://" + ip + (u.getPort() > 0 ? ":" + u.getPort() : "");
                    } catch (Exception ignored) { }
                    final String found = keep, ver = v;
                    ui.post(() -> { if (gen == searchGen) openRadar(found, ver.isEmpty() ? null : ver); });
                    return;
                }
            }
            ui.post(() -> { if (gen == searchGen) startDiscovery(gen); });
        });
    }

    private void startDiscovery(final int gen) {
        nsd = (NsdManager) getSystemService(Context.NSD_SERVICE);
        WifiManager wm = (WifiManager) getApplicationContext().getSystemService(Context.WIFI_SERVICE);
        if (wm != null) {
            mcLock = wm.createMulticastLock("lisasradar");
            mcLock.setReferenceCounted(false);
            mcLock.acquire();
        }
        final AtomicBoolean done = new AtomicBoolean(false);
        discovery = new NsdManager.DiscoveryListener() {
            @Override public void onStartDiscoveryFailed(String t, int e) { }
            @Override public void onStopDiscoveryFailed(String t, int e) { }
            @Override public void onDiscoveryStarted(String t) { }
            @Override public void onDiscoveryStopped(String t) { }
            @Override public void onServiceLost(NsdServiceInfo s) { }
            @Override
            public void onServiceFound(NsdServiceInfo s) {
                String n = s.getServiceName() == null ? "" : s.getServiceName().toLowerCase(Locale.ROOT);
                if (!n.contains("lisasradar") && !n.contains("capsuleradar")) return;
                resolve(s, gen, done);
            }
        };
        try {
            nsd.discoverServices("_http._tcp.", NsdManager.PROTOCOL_DNS_SD, discovery);
        } catch (Exception e) {
            discovery = null;
        }
        ui.postDelayed(() -> {
            if (gen != searchGen || done.get()) return;
            stopDiscovery();
            notFound();
        }, DISCOVERY_MS);
    }

    @SuppressWarnings("deprecation")   // resolveService: fine on every version we support
    private void resolve(NsdServiceInfo s, final int gen, final AtomicBoolean done) {
        try {
            nsd.resolveService(s, new NsdManager.ResolveListener() {
                @Override public void onResolveFailed(NsdServiceInfo si, int e) { }
                @Override
                public void onServiceResolved(NsdServiceInfo si) {
                    InetAddress h = si.getHost();
                    if (h == null) return;
                    final String b = "http://" + h.getHostAddress() + (si.getPort() != 80 && si.getPort() > 0 ? ":" + si.getPort() : "");
                    io.execute(() -> {
                        String v = probe(b);
                        if (v == null || gen != searchGen || !done.compareAndSet(false, true)) return;
                        ui.post(() -> {
                            stopDiscovery();
                            openRadar(b, v.isEmpty() ? null : v);
                        });
                    });
                }
            });
        } catch (Exception ignored) { }   // "already active": the next onServiceFound tries again
    }

    private void stopDiscovery() {
        if (nsd != null && discovery != null) {
            try { nsd.stopServiceDiscovery(discovery); } catch (Exception ignored) { }
        }
        discovery = null;
        if (mcLock != null && mcLock.isHeld()) mcLock.release();
    }

    private void notFound() {
        titleSub.setText("nicht gefunden");
        Button retry = makeButton("Erneut suchen", true);
        retry.setOnClickListener(v -> startSearch());
        Button addr = makeButton("Adresse eingeben", false);
        addr.setOnClickListener(v -> askAddress());
        showOverlay(false,
                "Das Radar wurde im WLAN nicht gefunden.\n\n"
                + "• Ist das Radar eingeschaltet und mit dem WLAN verbunden?\n"
                + "• Ist das Handy im selben WLAN (nicht nur mobile Daten)?\n\n"
                + "Die Adresse steht auch auf dem Radar: Einstellungen → QR-Code zur Webseite.",
                retry, addr);
    }

    private void askAddress() {
        final EditText in = new EditText(this);
        in.setInputType(InputType.TYPE_CLASS_TEXT | InputType.TYPE_TEXT_VARIATION_URI);
        in.setHint("z. B. 192.168.178.42");
        String cur = base != null ? base : prefs.getString(KEY_BASE, "");
        in.setText(cur.replace("http://", ""));
        FrameLayout box = new FrameLayout(this);
        box.setPadding(dp(20), dp(8), dp(20), 0);
        box.addView(in);
        new AlertDialog.Builder(this)
                .setTitle("Adresse des Radars")
                .setMessage("Die IP-Adresse oder lisasradar.local. Sie steht unter dem QR-Code in den Einstellungen am Radar.")
                .setView(box)
                .setPositiveButton("Verbinden", (d, w) -> {
                    String a = in.getText().toString().trim();
                    if (a.isEmpty()) return;
                    if (!a.startsWith("http://") && !a.startsWith("https://")) a = "http://" + a;
                    while (a.endsWith("/")) a = a.substring(0, a.length() - 1);
                    prefs.edit().putString(KEY_BASE, a).apply();
                    startSearch();
                })
                .setNeutralButton("Automatisch suchen", (d, w) -> {
                    prefs.edit().remove(KEY_BASE).apply();
                    startSearch();
                })
                .setNegativeButton("Abbrechen", null)
                .show();
    }

    // ------------------------------------------------------------------ location
    private boolean hasLocationPermission() {
        return checkSelfPermission(Manifest.permission.ACCESS_FINE_LOCATION) == PackageManager.PERMISSION_GRANTED
            || checkSelfPermission(Manifest.permission.ACCESS_COARSE_LOCATION) == PackageManager.PERMISSION_GRANTED;
    }

    private void onLocationButton() {
        if (base == null) return;
        if (!hasLocationPermission()) {
            requestPermissions(new String[]{Manifest.permission.ACCESS_FINE_LOCATION,
                                            Manifest.permission.ACCESS_COARSE_LOCATION}, REQ_LOCATION);
            return;
        }
        LocationManager lm = (LocationManager) getSystemService(Context.LOCATION_SERVICE);
        if (lm == null) { toast("Standort ist auf diesem Handy nicht verfügbar."); return; }
        if (Build.VERSION.SDK_INT >= 28 && !lm.isLocationEnabled()) {
            new AlertDialog.Builder(this)
                    .setTitle("Standort ist aus")
                    .setMessage("Bitte die Standortermittlung des Handys einschalten und dann noch einmal tippen.")
                    .setPositiveButton("Einstellungen öffnen", (d, w) ->
                            startActivity(new Intent(Settings.ACTION_LOCATION_SOURCE_SETTINGS)))
                    .setNegativeButton("Abbrechen", null)
                    .show();
            return;
        }
        getFix(lm);
    }

    @Override
    public void onRequestPermissionsResult(int req, String[] perms, int[] results) {
        super.onRequestPermissionsResult(req, perms, results);
        if (req != REQ_LOCATION) return;
        if (hasLocationPermission()) onLocationButton();
        else toast("Ohne Standort-Erlaubnis kann die App den Standort nicht senden.");
    }

    @SuppressWarnings({"MissingPermission", "deprecation"})
    private void getFix(LocationManager lm) {
        // a recent, good last-known fix is good enough (and instant)
        Location best = null;
        for (String p : lm.getProviders(true)) {
            try {
                Location l = lm.getLastKnownLocation(p);
                if (l == null) continue;
                long ageMs = (android.os.SystemClock.elapsedRealtimeNanos() - l.getElapsedRealtimeNanos()) / 1_000_000L;
                if (ageMs > 2 * 60_000L || !l.hasAccuracy() || l.getAccuracy() > 100f) continue;
                if (best == null || l.getAccuracy() < best.getAccuracy()) best = l;
            } catch (Exception ignored) { }
        }
        if (best != null) { confirmAndSend(best); return; }

        String provider = pickProvider(lm);
        if (provider == null) { toast("Kein Standortanbieter verfügbar."); return; }
        busyDialog = new AlertDialog.Builder(this)
                .setTitle("Standort wird bestimmt ...")
                .setMessage("Einen Moment -- am besten in der Nähe eines Fensters.")
                .setNegativeButton("Abbrechen", (d, w) -> cancelFix())
                .setCancelable(false)
                .show();
        final AtomicBoolean answered = new AtomicBoolean(false);
        if (Build.VERSION.SDK_INT >= 30) {
            fixCancel = new CancellationSignal();
            lm.getCurrentLocation(provider, fixCancel, getMainExecutor(), l -> {
                if (answered.compareAndSet(false, true)) onFix(l);
            });
        } else {
            // an anonymous class, not a lambda: before Android 11 the other three methods are
            // abstract in the framework and get called -- a lambda would crash there
            legacyListener = new LocationListener() {
                @Override public void onLocationChanged(Location l) { if (answered.compareAndSet(false, true)) onFix(l); }
                @Override public void onStatusChanged(String p, int s, Bundle b) { }
                @Override public void onProviderEnabled(String p) { }
                @Override public void onProviderDisabled(String p) { }
            };
            lm.requestSingleUpdate(provider, legacyListener, Looper.getMainLooper());
        }
        ui.postDelayed(() -> {
            if (!answered.compareAndSet(false, true)) return;
            cancelFix();
            Location last = null;
            try { last = lm.getLastKnownLocation(provider); } catch (Exception ignored) { }
            onFix(last);
        }, FIX_TIMEOUT_MS);
    }

    private static String pickProvider(LocationManager lm) {
        List<String> on = lm.getProviders(true);
        if (Build.VERSION.SDK_INT >= 31 && on.contains(LocationManager.FUSED_PROVIDER)) return LocationManager.FUSED_PROVIDER;
        if (on.contains(LocationManager.GPS_PROVIDER)) return LocationManager.GPS_PROVIDER;
        if (on.contains(LocationManager.NETWORK_PROVIDER)) return LocationManager.NETWORK_PROVIDER;
        return on.isEmpty() ? null : on.get(0);
    }

    private void cancelFix() {
        if (fixCancel != null) { fixCancel.cancel(); fixCancel = null; }
        if (legacyListener != null) {
            LocationManager lm = (LocationManager) getSystemService(Context.LOCATION_SERVICE);
            if (lm != null) lm.removeUpdates(legacyListener);
            legacyListener = null;
        }
        if (busyDialog != null) { busyDialog.dismiss(); busyDialog = null; }
    }

    private void onFix(Location l) {
        cancelFix();
        if (l == null) {
            toast("Kein Standort gefunden. Bitte am Fenster oder draußen noch einmal versuchen.");
            return;
        }
        confirmAndSend(l);
    }

    private void confirmAndSend(final Location l) {
        final double lat = l.getLatitude(), lon = l.getLongitude();
        String acc = l.hasAccuracy() ? String.format(Locale.GERMANY, "\nGenauigkeit: etwa %.0f m", l.getAccuracy()) : "";
        new AlertDialog.Builder(this)
                .setTitle("Standort ans Radar senden?")
                .setMessage(String.format(Locale.GERMANY, "%.5f, %.5f", lat, lon) + acc
                        + "\n\nDas Radar zeigt danach die Flugzeuge rund um diesen Ort. "
                        + "Der Ort wird auf dem Radar gespeichert.")
                .setPositiveButton("Senden", (d, w) -> sendLocation(lat, lon))
                .setNegativeButton("Abbrechen", null)
                .show();
    }

    private void sendLocation(final double lat, final double lon) {
        final String b = base;
        if (b == null) return;
        setLocEnabled(false);
        io.execute(() -> {
            int code = -1;
            String body = "";
            try {
                HttpURLConnection c = (HttpURLConnection) new URL(b + "/loc").openConnection();
                c.setConnectTimeout(4000);
                c.setReadTimeout(6000);
                c.setRequestMethod("POST");
                c.setDoOutput(true);
                c.setRequestProperty("X-Radar", "1");
                c.setRequestProperty("Content-Type", "application/x-www-form-urlencoded");
                byte[] form = String.format(Locale.US, "lat=%.6f&lon=%.6f", lat, lon).getBytes(StandardCharsets.UTF_8);
                c.setFixedLengthStreamingMode(form.length);
                try (OutputStream o = c.getOutputStream()) { o.write(form); }
                code = c.getResponseCode();
                if (code == 200) body = readAll(c.getInputStream());
            } catch (Exception ignored) { }
            final int fc = code;
            final String fb = body;
            ui.post(() -> {
                setLocEnabled(base != null);
                if (fc == 200) {
                    toast(fb.contains("\"gpsOff\":true")
                            ? "Standort gespeichert ✓\n(„GPS für den Standort“ wurde dafür ausgeschaltet.)"
                            : "Standort gespeichert ✓ Das Radar zeigt jetzt diesen Ort.");
                    web.reload();               // the page shows the new latitude/longitude
                } else if (fc == 404) {
                    new AlertDialog.Builder(this)
                            .setTitle("Firmware zu alt")
                            .setMessage("Das Radar kennt diese Funktion noch nicht. Bitte zuerst die neue Firmware "
                                    + "(1.8.0-de13 oder neuer) aufspielen: auf der Radar-Seite ganz unten „Firmware-Update“.")
                            .setPositiveButton("OK", null)
                            .show();
                } else {
                    toast("Senden hat nicht geklappt" + (fc > 0 ? " (Fehler " + fc + ")" : "") + ". Ist das Radar im WLAN?");
                }
            });
        });
    }
}
