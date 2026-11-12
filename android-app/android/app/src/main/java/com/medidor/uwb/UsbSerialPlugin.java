package com.medidor.uwb;

import android.Manifest;
import android.app.PendingIntent;
import android.content.BroadcastReceiver;
import android.content.ComponentName;
import android.content.Context;
import android.content.Intent;
import android.content.IntentFilter;
import android.content.ServiceConnection;
import android.content.pm.PackageManager;
import android.hardware.usb.UsbDevice;
import android.hardware.usb.UsbDeviceConnection;
import android.hardware.usb.UsbManager;
import android.net.Uri;
import android.os.Build;
import android.os.IBinder;
import android.os.PowerManager;
import android.provider.Settings;
import android.util.Log;

import androidx.core.app.ActivityCompat;
import androidx.core.content.ContextCompat;

import com.getcapacitor.JSArray;
import com.getcapacitor.JSObject;
import com.getcapacitor.Plugin;
import com.getcapacitor.PluginCall;
import com.getcapacitor.PluginMethod;
import com.getcapacitor.annotation.CapacitorPlugin;
import com.hoho.android.usbserial.driver.UsbSerialDriver;
import com.hoho.android.usbserial.driver.UsbSerialPort;
import com.hoho.android.usbserial.driver.UsbSerialProber;

import java.io.IOException;
import java.nio.charset.StandardCharsets;
import java.util.HashMap;
import java.util.List;

/**
 * Ponte entre o JS e o {@link SerialService}. O plugin nao segura mais a porta:
 * ele apenas manda abrir/fechar/escrever e se registra como listener enquanto a
 * UI esta visivel. Com o app em segundo plano o servico assume sozinho.
 */
@CapacitorPlugin(name = "UsbSerial")
public class UsbSerialPlugin extends Plugin implements SerialService.Listener {
    private static final String TAG = "UsbSerialPlugin";
    private static final String ACTION_USB_PERMISSION = "com.medidor.uwb.USB_PERMISSION";
    private static final int NOTIFICATION_PERMISSION_REQUEST = 4001;
    private static final int WRITE_TIMEOUT_MS = 1000;

    private UsbManager usbManager;
    private SerialService service;
    private boolean uiVisible;

    private final ServiceConnection connection = new ServiceConnection() {
        @Override
        public void onServiceConnected(ComponentName name, IBinder binder) {
            service = ((SerialService.SerialBinder) binder).getService();
            Log.d(TAG, "SerialService conectado");
            if (uiVisible) {
                service.attach(UsbSerialPlugin.this);
            }
        }

        @Override
        public void onServiceDisconnected(ComponentName name) {
            Log.w(TAG, "SerialService desconectado");
            service = null;
        }
    };

    // ------------------------------------------------------------- ciclo de vida

    @Override
    public void load() {
        super.load();
        usbManager = (UsbManager) getContext().getSystemService(Context.USB_SERVICE);

        Log.d(TAG, "UsbSerialPlugin loaded!");
        HashMap<String, UsbDevice> deviceList = usbManager.getDeviceList();
        Log.d(TAG, "USB devices on load: " + deviceList.size());
        for (UsbDevice device : deviceList.values()) {
            Log.d(TAG, "USB Device on load: " + device.getDeviceName() +
                  " VID:" + String.format("%04X", device.getVendorId()) +
                  " PID:" + String.format("%04X", device.getProductId()));
        }

        // startService + bindService: o start faz o servico sobreviver ao unbind
        // quando a Activity morre com a porta aberta.
        Intent intent = new Intent(getContext(), SerialService.class);
        getContext().startService(intent);
        getContext().bindService(intent, connection, Context.BIND_AUTO_CREATE);
    }

    @Override
    protected void handleOnStart() {
        super.handleOnStart();
        uiVisible = true;
        if (service != null) {
            service.attach(this);
        }
    }

    @Override
    protected void handleOnStop() {
        super.handleOnStop();
        uiVisible = false;
        if (service != null) {
            // A porta continua aberta; o servico vira foreground e segura os dados.
            service.detach();
        }
    }

    @Override
    protected void handleOnDestroy() {
        if (service != null) {
            service.detach();
        }
        try {
            getContext().unbindService(connection);
        } catch (IllegalArgumentException e) {
            Log.w(TAG, "Servico nao estava bound", e);
        }
        service = null;
        super.handleOnDestroy();
    }

    // ------------------------------------------------------------- descoberta

    @PluginMethod
    public void listDevices(PluginCall call) {
        Log.d(TAG, "listDevices called");

        HashMap<String, UsbDevice> deviceList = usbManager.getDeviceList();
        Log.d(TAG, "Total USB devices connected: " + deviceList.size());

        List<UsbSerialDriver> availableDrivers = UsbSerialProber.getDefaultProber().findAllDrivers(usbManager);
        Log.d(TAG, "Found " + availableDrivers.size() + " USB SERIAL devices");

        JSArray devices = new JSArray();
        StringBuilder deviceInfo = new StringBuilder();

        for (int i = 0; i < availableDrivers.size(); i++) {
            UsbSerialDriver driver = availableDrivers.get(i);
            UsbDevice device = driver.getDevice();

            JSObject item = new JSObject();
            item.put("index", i);
            item.put("vendorId", device.getVendorId());
            item.put("productId", device.getProductId());
            item.put("deviceName", device.getDeviceName());
            item.put("driver", driver.getClass().getSimpleName());
            item.put("hasPermission", usbManager.hasPermission(device));
            devices.put(item);

            if (i > 0) {
                deviceInfo.append("\n");
            }
            deviceInfo.append("Device ").append(i + 1).append(": ")
                      .append(device.getDeviceName())
                      .append(" (VID: ").append(String.format("%04X", device.getVendorId()))
                      .append(", PID: ").append(String.format("%04X", device.getProductId()))
                      .append(")");

            Log.d(TAG, "Serial Device " + i + ": " + device.getDeviceName() +
                  " VID:" + String.format("%04X", device.getVendorId()) +
                  " PID:" + String.format("%04X", device.getProductId()));
        }

        JSObject ret = new JSObject();
        ret.put("count", availableDrivers.size());
        ret.put("devices", devices);
        if (deviceInfo.length() > 0) {
            ret.put("info", deviceInfo.toString());
        }
        call.resolve(ret);
    }

    // Seleciona o driver por vendorId/productId quando informados, senao o primeiro disponivel.
    private UsbSerialDriver findDriver(PluginCall call) {
        List<UsbSerialDriver> availableDrivers = UsbSerialProber.getDefaultProber().findAllDrivers(usbManager);

        if (availableDrivers.isEmpty()) {
            return null;
        }

        Integer vendorId = call.getInt("vendorId");
        Integer productId = call.getInt("productId");

        if (vendorId != null) {
            for (UsbSerialDriver driver : availableDrivers) {
                UsbDevice device = driver.getDevice();
                if (device.getVendorId() == vendorId &&
                    (productId == null || device.getProductId() == productId)) {
                    return driver;
                }
            }
            return null;
        }

        return availableDrivers.get(0);
    }

    @PluginMethod
    public void requestPermission(PluginCall call) {
        Log.d(TAG, "requestPermission called");

        UsbSerialDriver driver = findDriver(call);
        if (driver == null) {
            Log.e(TAG, "No USB devices found");
            call.reject("No USB devices found");
            return;
        }

        UsbDevice device = driver.getDevice();
        Log.d(TAG, "Found device: " + device.getDeviceName());

        if (usbManager.hasPermission(device)) {
            Log.d(TAG, "Permission already granted");
            JSObject ret = new JSObject();
            ret.put("granted", true);
            call.resolve(ret);
            return;
        }

        Log.d(TAG, "Requesting permission");

        Intent intent = new Intent(ACTION_USB_PERMISSION);
        intent.setPackage(getContext().getPackageName()); // Intent explicito para Android 14+

        PendingIntent permissionIntent = PendingIntent.getBroadcast(
            getContext(),
            0,
            intent,
            PendingIntent.FLAG_MUTABLE | PendingIntent.FLAG_UPDATE_CURRENT
        );

        IntentFilter filter = new IntentFilter(ACTION_USB_PERMISSION);
        getContext().registerReceiver(new BroadcastReceiver() {
            @Override
            public void onReceive(Context context, Intent intent) {
                if (ACTION_USB_PERMISSION.equals(intent.getAction())) {
                    synchronized (this) {
                        UsbDevice target = intent.getParcelableExtra(UsbManager.EXTRA_DEVICE);

                        if (intent.getBooleanExtra(UsbManager.EXTRA_PERMISSION_GRANTED, false)) {
                            Log.d(TAG, "Permission granted for device " + target);
                            JSObject ret = new JSObject();
                            ret.put("granted", true);
                            call.resolve(ret);
                        } else {
                            Log.d(TAG, "Permission denied for device " + target);
                            call.reject("Permission denied");
                        }
                        context.unregisterReceiver(this);
                    }
                }
            }
        }, filter, Context.RECEIVER_NOT_EXPORTED);

        usbManager.requestPermission(device, permissionIntent);
    }

    // ------------------------------------------------------------- porta

    @PluginMethod
    public void open(PluginCall call) {
        int baudRate = call.getInt("baudRate", 115200);
        int dataBits = call.getInt("dataBits", 8);
        int stopBits = call.getInt("stopBits", UsbSerialPort.STOPBITS_1);
        int parity = call.getInt("parity", UsbSerialPort.PARITY_NONE);

        Log.d(TAG, "open called with baudRate: " + baudRate);

        if (service == null) {
            call.reject("Servico serial ainda nao esta pronto");
            return;
        }

        // Sem POST_NOTIFICATIONS a notificacao do foreground service nao aparece
        // (o servico roda mesmo assim, mas o usuario fica sem o botao Desconectar).
        ensureNotificationPermission();

        try {
            UsbSerialDriver driver = findDriver(call);
            if (driver == null) {
                call.reject("No USB devices found");
                return;
            }

            UsbDeviceConnection connection = usbManager.openDevice(driver.getDevice());
            if (connection == null) {
                call.reject("Failed to open device connection. Permission may not be granted.");
                return;
            }

            service.openPort(driver, connection, baudRate, dataBits, stopBits, parity);

            Log.d(TAG, "Serial port opened successfully");

            JSObject ret = new JSObject();
            ret.put("success", true);
            call.resolve(ret);

        } catch (IOException e) {
            Log.e(TAG, "Error opening serial port", e);
            call.reject("Error opening serial port: " + e.getMessage());
        }
    }

    @PluginMethod
    public void write(PluginCall call) {
        String data = call.getString("data");

        if (service == null || !service.isOpen()) {
            call.reject("Serial port not open");
            return;
        }
        if (data == null) {
            call.reject("Missing 'data'");
            return;
        }

        try {
            byte[] bytes = data.getBytes(StandardCharsets.UTF_8);

            Log.d(TAG, "=== WRITE DEBUG ===");
            Log.d(TAG, "String: " + data);
            Log.d(TAG, "Length: " + data.length());
            Log.d(TAG, "Bytes: " + bytesToHex(bytes));

            service.write(bytes, WRITE_TIMEOUT_MS);

            Log.d(TAG, "Write successful");

            JSObject ret = new JSObject();
            ret.put("success", true);
            call.resolve(ret);
        } catch (IOException e) {
            Log.e(TAG, "Error writing to serial port", e);
            call.reject("Error writing: " + e.getMessage());
        }
    }

    // Envia bytes crus a partir de uma string hex ("01 FF 0A" ou "01FF0A").
    @PluginMethod
    public void writeHex(PluginCall call) {
        String hex = call.getString("data");

        if (service == null || !service.isOpen()) {
            call.reject("Serial port not open");
            return;
        }
        if (hex == null) {
            call.reject("Missing 'data'");
            return;
        }

        String clean = hex.replaceAll("[^0-9a-fA-F]", "");
        if (clean.length() % 2 != 0) {
            call.reject("Hex string must have an even number of digits");
            return;
        }

        byte[] bytes = new byte[clean.length() / 2];
        for (int i = 0; i < bytes.length; i++) {
            bytes[i] = (byte) Integer.parseInt(clean.substring(i * 2, i * 2 + 2), 16);
        }

        try {
            Log.d(TAG, "Write HEX: " + bytesToHex(bytes));
            service.write(bytes, WRITE_TIMEOUT_MS);

            JSObject ret = new JSObject();
            ret.put("success", true);
            ret.put("bytesWritten", bytes.length);
            call.resolve(ret);
        } catch (IOException e) {
            Log.e(TAG, "Error writing hex to serial port", e);
            call.reject("Error writing: " + e.getMessage());
        }
    }

    // Estado atual da porta, para o JS se ressincronizar apos voltar do background.
    @PluginMethod
    public void getStatus(PluginCall call) {
        boolean isOpen = service != null && service.isOpen();

        JSObject ret = new JSObject();
        ret.put("isOpen", isOpen);

        if (isOpen) {
            UsbDevice device = service.getPort().getDriver().getDevice();
            ret.put("vendorId", device.getVendorId());
            ret.put("productId", device.getProductId());
            ret.put("deviceName", device.getDeviceName());
        }

        ret.put("buffered", service == null ? 0 : service.bufferedLength());
        call.resolve(ret);
    }

    // Consome o que chegou desde a ultima chamada (polling); eventos 'dataReceived' cobrem o modo push.
    @PluginMethod
    public void read(PluginCall call) {
        if (service == null || !service.isOpen()) {
            call.reject("Serial port not open");
            return;
        }

        JSObject ret = new JSObject();
        ret.put("data", service.drainBuffer());
        call.resolve(ret);
    }

    @PluginMethod
    public void close(PluginCall call) {
        if (service != null) {
            service.closePort();
        }
        JSObject ret = new JSObject();
        ret.put("success", true);
        call.resolve(ret);
    }

    // ------------------------------------------------------------- background

    /**
     * Diz se o app esta isento das otimizacoes de bateria. Sem a isencao, o
     * fabricante ainda pode matar o processo depois de horas em segundo plano,
     * mesmo com foreground service.
     */
    @PluginMethod
    public void getBackgroundStatus(PluginCall call) {
        PowerManager pm = (PowerManager) getContext().getSystemService(Context.POWER_SERVICE);
        boolean ignoring = pm != null && pm.isIgnoringBatteryOptimizations(getContext().getPackageName());

        boolean notifications = Build.VERSION.SDK_INT < Build.VERSION_CODES.TIRAMISU ||
            ContextCompat.checkSelfPermission(getContext(), Manifest.permission.POST_NOTIFICATIONS)
                == PackageManager.PERMISSION_GRANTED;

        JSObject ret = new JSObject();
        ret.put("serviceBound", service != null);
        ret.put("ignoringBatteryOptimizations", ignoring);
        ret.put("notificationsGranted", notifications);
        call.resolve(ret);
    }

    /** Abre a tela do sistema onde o usuario libera o app das otimizacoes de bateria. */
    @PluginMethod
    public void requestIgnoreBatteryOptimizations(PluginCall call) {
        PowerManager pm = (PowerManager) getContext().getSystemService(Context.POWER_SERVICE);
        String pkg = getContext().getPackageName();

        if (pm != null && pm.isIgnoringBatteryOptimizations(pkg)) {
            JSObject ret = new JSObject();
            ret.put("granted", true);
            call.resolve(ret);
            return;
        }

        try {
            Intent intent = new Intent(Settings.ACTION_REQUEST_IGNORE_BATTERY_OPTIMIZATIONS)
                .setData(Uri.parse("package:" + pkg));
            getActivity().startActivity(intent);
        } catch (Exception e) {
            // Alguns aparelhos bloqueiam o atalho direto; cai na lista geral.
            Log.w(TAG, "Atalho direto indisponivel, abrindo a lista", e);
            try {
                getActivity().startActivity(new Intent(Settings.ACTION_IGNORE_BATTERY_OPTIMIZATION_SETTINGS));
            } catch (Exception inner) {
                call.reject("Nao foi possivel abrir as configuracoes de bateria: " + inner.getMessage());
                return;
            }
        }

        JSObject ret = new JSObject();
        ret.put("granted", false);
        call.resolve(ret);
    }

    private void ensureNotificationPermission() {
        if (Build.VERSION.SDK_INT < Build.VERSION_CODES.TIRAMISU) return;
        if (ContextCompat.checkSelfPermission(getContext(), Manifest.permission.POST_NOTIFICATIONS)
                == PackageManager.PERMISSION_GRANTED) {
            return;
        }

        ActivityCompat.requestPermissions(
            getActivity(),
            new String[]{Manifest.permission.POST_NOTIFICATIONS},
            NOTIFICATION_PERMISSION_REQUEST
        );
    }

    // ------------------------------------------------------------- eventos do servico

    @Override
    public void onSerialData(String data) {
        JSObject event = new JSObject();
        event.put("data", data);
        notifyListeners("dataReceived", event);
    }

    @Override
    public void onSerialError(String message) {
        JSObject event = new JSObject();
        event.put("error", message);
        notifyListeners("readError", event);
    }

    @Override
    public void onDeviceAttached(UsbDevice device) {
        notifyListeners("deviceAttached", deviceEvent(device));
    }

    @Override
    public void onDeviceDetached(UsbDevice device) {
        notifyListeners("deviceDetached", deviceEvent(device));
    }

    private JSObject deviceEvent(UsbDevice device) {
        JSObject event = new JSObject();
        if (device != null) {
            event.put("vendorId", device.getVendorId());
            event.put("productId", device.getProductId());
            event.put("deviceName", device.getDeviceName());
        }
        return event;
    }

    private String bytesToHex(byte[] bytes) {
        StringBuilder sb = new StringBuilder();
        for (byte b : bytes) {
            sb.append(String.format("%02X ", b));
        }
        return sb.toString();
    }
}
