package com.medidor.uwb;

import android.app.Notification;
import android.app.NotificationChannel;
import android.app.NotificationManager;
import android.app.PendingIntent;
import android.app.Service;
import android.content.BroadcastReceiver;
import android.content.Context;
import android.content.Intent;
import android.content.IntentFilter;
import android.content.pm.ServiceInfo;
import android.hardware.usb.UsbDevice;
import android.hardware.usb.UsbDeviceConnection;
import android.hardware.usb.UsbManager;
import android.os.Binder;
import android.os.Build;
import android.os.Handler;
import android.os.IBinder;
import android.os.Looper;
import android.os.PowerManager;
import android.util.Log;

import androidx.core.app.NotificationCompat;
import androidx.core.content.ContextCompat;

import com.hoho.android.usbserial.driver.UsbSerialDriver;
import com.hoho.android.usbserial.driver.UsbSerialPort;
import com.hoho.android.usbserial.util.SerialInputOutputManager;

import java.io.IOException;
import java.nio.charset.StandardCharsets;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;

/**
 * Dono da porta serial. Fica fora da Activity para que a conexao sobreviva ao
 * app ir para segundo plano: enquanto a UI esta visivel o servico roda apenas
 * como bound service; quando ela se desconecta e ha porta aberta, ele vira
 * foreground service do tipo connectedDevice (sem limite de 6 h no Android 15)
 * e segura um PARTIAL_WAKE_LOCK, para o Doze nao suspender a CPU no meio da
 * leitura.
 *
 * Cadeia de eventos: SerialInputOutputManager -> SerialService -> UsbSerialPlugin -> JS.
 */
public class SerialService extends Service implements SerialInputOutputManager.Listener {

    /** A UI (plugin Capacitor) implementa isto para receber os eventos da porta. */
    public interface Listener {
        void onSerialData(String data);
        void onSerialError(String message);
        void onDeviceAttached(UsbDevice device);
        void onDeviceDetached(UsbDevice device);
    }

    public class SerialBinder extends Binder {
        SerialService getService() {
            return SerialService.this;
        }
    }

    static final String ACTION_DISCONNECT = "com.medidor.uwb.DISCONNECT";

    private static final String TAG = "SerialService";
    private static final String CHANNEL_ID = "serial_connection";
    private static final int NOTIFY_ID = 1001;
    private static final int MAX_BUFFER = 8192;
    private static final String WAKE_LOCK_TAG = "MedidorUWB:serial";

    private final IBinder binder = new SerialBinder();
    private final StringBuilder readBuffer = new StringBuilder();
    private final Handler mainHandler = new Handler(Looper.getMainLooper());

    private UsbSerialPort serialPort;
    private SerialInputOutputManager ioManager;
    private ExecutorService ioExecutor;
    private PowerManager.WakeLock wakeLock;
    private BroadcastReceiver usbReceiver;

    /** null enquanto a UI esta em segundo plano. */
    private Listener listener;
    private boolean foreground;
    private String deviceLabel = "";

    // ------------------------------------------------------------- ciclo de vida

    @Override
    public void onCreate() {
        super.onCreate();
        createChannel();
        registerUsbReceiver();
    }

    @Override
    public IBinder onBind(Intent intent) {
        return binder;
    }

    @Override
    public int onStartCommand(Intent intent, int flags, int startId) {
        if (intent != null && ACTION_DISCONNECT.equals(intent.getAction())) {
            Log.d(TAG, "Desconexao pedida pela notificacao");
            closePort();
        }
        // O servico e mantido vivo pelo bind + foreground, nao por reinicio automatico.
        return START_NOT_STICKY;
    }

    @Override
    public void onDestroy() {
        if (usbReceiver != null) {
            try {
                unregisterReceiver(usbReceiver);
            } catch (IllegalArgumentException e) {
                Log.w(TAG, "Receiver USB nao estava registrado", e);
            }
            usbReceiver = null;
        }
        closePort();
        super.onDestroy();
    }

    // ------------------------------------------------------------- UI attach/detach

    /** Chamado quando a UI volta ao primeiro plano; devolve o que chegou enquanto ela esteve fora. */
    public void attach(Listener newListener) {
        listener = newListener;
        stopForegroundMode();

        String pending;
        synchronized (readBuffer) {
            pending = readBuffer.toString();
            readBuffer.setLength(0);
        }
        if (!pending.isEmpty()) {
            Log.d(TAG, "Entregando " + pending.length() + " chars acumulados no background");
            newListener.onSerialData(pending);
        }
    }

    /** Chamado quando a UI vai para segundo plano; a porta continua aberta. */
    public void detach() {
        listener = null;
        if (isOpen()) {
            startForegroundMode();
        }
    }

    // ------------------------------------------------------------- porta serial

    public void openPort(UsbSerialDriver driver, UsbDeviceConnection connection,
                         int baudRate, int dataBits, int stopBits, int parity) throws IOException {
        closePort();

        UsbSerialPort port = driver.getPorts().get(0);
        port.open(connection);
        port.setParameters(baudRate, dataBits, stopBits, parity);
        try {
            port.setDTR(true);
            port.setRTS(true);
        } catch (Exception e) {
            Log.w(TAG, "DTR/RTS nao suportado por este driver", e);
        }

        serialPort = port;
        UsbDevice device = driver.getDevice();
        deviceLabel = String.format("%04X:%04X", device.getVendorId(), device.getProductId());

        synchronized (readBuffer) {
            readBuffer.setLength(0);
        }

        ioManager = new SerialInputOutputManager(port, this);
        ioExecutor = Executors.newSingleThreadExecutor();
        ioExecutor.submit(ioManager);

        acquireWakeLock();
        if (listener == null) {
            // Aberta com a UI ja em background (auto-reconexao): sobe direto em foreground.
            startForegroundMode();
        }

        Log.d(TAG, "Porta aberta: " + deviceLabel + " @ " + baudRate);
    }

    public void closePort() {
        if (ioManager != null) {
            ioManager.setListener(null);
            ioManager.stop();
            ioManager = null;
        }
        if (ioExecutor != null) {
            ioExecutor.shutdown();
            ioExecutor = null;
        }
        if (serialPort != null) {
            try {
                serialPort.close();
            } catch (IOException e) {
                Log.e(TAG, "Erro ao fechar a porta", e);
            }
            serialPort = null;
        }

        releaseWakeLock();
        stopForegroundMode();
    }

    public boolean isOpen() {
        return serialPort != null;
    }

    public UsbSerialPort getPort() {
        return serialPort;
    }

    public void write(byte[] bytes, int timeoutMs) throws IOException {
        UsbSerialPort port = serialPort;
        if (port == null) {
            throw new IOException("Porta serial nao esta aberta");
        }
        port.write(bytes, timeoutMs);
    }

    /** Consome o buffer acumulado (modo polling do JS). */
    public String drainBuffer() {
        synchronized (readBuffer) {
            String data = readBuffer.toString();
            readBuffer.setLength(0);
            return data;
        }
    }

    public int bufferedLength() {
        synchronized (readBuffer) {
            return readBuffer.length();
        }
    }

    // ------------------------------------------------------------- callbacks da lib

    @Override
    public void onNewData(byte[] data) {
        final String text = new String(data, StandardCharsets.UTF_8);
        final Listener target = listener;

        if (target == null) {
            // UI em background: acumula respeitando o teto do buffer.
            synchronized (readBuffer) {
                readBuffer.append(text);
                if (readBuffer.length() > MAX_BUFFER) {
                    readBuffer.delete(0, readBuffer.length() - MAX_BUFFER);
                }
            }
            return;
        }

        mainHandler.post(() -> target.onSerialData(text));
    }

    @Override
    public void onRunError(Exception e) {
        Log.e(TAG, "Erro de leitura na serial", e);
        final String message = e.getMessage() == null ? "Serial read error" : e.getMessage();
        final Listener target = listener;

        // A thread de leitura morreu, a porta nao serve mais.
        closePort();

        if (target != null) {
            mainHandler.post(() -> target.onSerialError(message));
        }
    }

    // ------------------------------------------------------------- USB attach/detach

    private void registerUsbReceiver() {
        usbReceiver = new BroadcastReceiver() {
            @Override
            public void onReceive(Context context, Intent intent) {
                UsbDevice device = intent.getParcelableExtra(UsbManager.EXTRA_DEVICE);
                final Listener target = listener;

                if (UsbManager.ACTION_USB_DEVICE_ATTACHED.equals(intent.getAction())) {
                    Log.d(TAG, "USB conectado: " + device);
                    if (target != null) mainHandler.post(() -> target.onDeviceAttached(device));
                } else if (UsbManager.ACTION_USB_DEVICE_DETACHED.equals(intent.getAction())) {
                    Log.d(TAG, "USB removido: " + device);
                    closePort();  // o cabo saiu, a porta morreu junto
                    if (target != null) mainHandler.post(() -> target.onDeviceDetached(device));
                }
            }
        };

        IntentFilter filter = new IntentFilter();
        filter.addAction(UsbManager.ACTION_USB_DEVICE_ATTACHED);
        filter.addAction(UsbManager.ACTION_USB_DEVICE_DETACHED);
        ContextCompat.registerReceiver(this, usbReceiver, filter, ContextCompat.RECEIVER_NOT_EXPORTED);
    }

    // ------------------------------------------------------------- foreground + wake lock

    private void createChannel() {
        if (Build.VERSION.SDK_INT < Build.VERSION_CODES.O) return;

        NotificationChannel channel = new NotificationChannel(
            CHANNEL_ID,
            "Conexao serial",
            NotificationManager.IMPORTANCE_LOW  // sem som nem vibracao
        );
        channel.setDescription("Mantem a porta USB aberta com o app em segundo plano");
        channel.setShowBadge(false);

        NotificationManager manager = getSystemService(NotificationManager.class);
        if (manager != null) manager.createNotificationChannel(channel);
    }

    private Notification buildNotification() {
        Intent openApp = new Intent(this, MainActivity.class)
            .setFlags(Intent.FLAG_ACTIVITY_SINGLE_TOP | Intent.FLAG_ACTIVITY_CLEAR_TOP);
        PendingIntent contentIntent = PendingIntent.getActivity(
            this, 0, openApp, PendingIntent.FLAG_IMMUTABLE | PendingIntent.FLAG_UPDATE_CURRENT);

        Intent disconnect = new Intent(this, SerialService.class).setAction(ACTION_DISCONNECT);
        PendingIntent disconnectIntent = PendingIntent.getService(
            this, 1, disconnect, PendingIntent.FLAG_IMMUTABLE | PendingIntent.FLAG_UPDATE_CURRENT);

        return new NotificationCompat.Builder(this, CHANNEL_ID)
            .setSmallIcon(android.R.drawable.stat_sys_data_bluetooth)
            .setContentTitle("Porta serial conectada")
            .setContentText("Dispositivo " + deviceLabel + " - recebendo dados")
            .setPriority(NotificationCompat.PRIORITY_LOW)
            .setCategory(NotificationCompat.CATEGORY_SERVICE)
            .setOngoing(true)
            .setShowWhen(false)
            .setContentIntent(contentIntent)
            .addAction(android.R.drawable.ic_menu_close_clear_cancel, "Desconectar", disconnectIntent)
            .build();
    }

    private void startForegroundMode() {
        if (foreground) return;

        try {
            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.UPSIDE_DOWN_CAKE) {
                startForeground(NOTIFY_ID, buildNotification(),
                                ServiceInfo.FOREGROUND_SERVICE_TYPE_CONNECTED_DEVICE);
            } else {
                startForeground(NOTIFY_ID, buildNotification());
            }
            foreground = true;
            Log.d(TAG, "Foreground service ativo, conexao mantida em segundo plano");
        } catch (Exception e) {
            // Ex.: ForegroundServiceStartNotAllowedException com o app ja em background restrito.
            Log.e(TAG, "Nao foi possivel entrar em foreground", e);
        }
    }

    private void stopForegroundMode() {
        if (!foreground) return;

        stopForeground(STOP_FOREGROUND_REMOVE);
        foreground = false;
    }

    /** O foreground service segura o processo; o wake lock segura a CPU durante o Doze. */
    private void acquireWakeLock() {
        if (wakeLock != null && wakeLock.isHeld()) return;

        PowerManager pm = (PowerManager) getSystemService(Context.POWER_SERVICE);
        if (pm == null) return;

        wakeLock = pm.newWakeLock(PowerManager.PARTIAL_WAKE_LOCK, WAKE_LOCK_TAG);
        wakeLock.setReferenceCounted(false);
        wakeLock.acquire();
    }

    private void releaseWakeLock() {
        if (wakeLock == null) return;

        if (wakeLock.isHeld()) wakeLock.release();
        wakeLock = null;
    }
}
