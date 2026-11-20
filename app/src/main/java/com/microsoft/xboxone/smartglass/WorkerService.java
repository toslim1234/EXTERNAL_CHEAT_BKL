package com.microsoft.xboxone.smartglass;

import android.app.Notification;
import android.app.NotificationChannel;
import android.app.NotificationManager;
import android.app.Service;
import android.content.Context;
import android.content.Intent;
import android.os.Build;
import android.os.IBinder;

/**
 * Keeps the overlay process alive while the game is in the foreground.
 *
 * Without a foreground service the system treats us as a cached process and
 * reclaims us as soon as the game needs RAM — the ESP would vanish after a few
 * seconds. The notification is IMPORTANCE_MIN and carries no text worth reading.
 */
public class WorkerService extends Service {

    private static final String CHANNEL = "worker";
    private static final int NOTIFICATION_ID = 0x42;

    public static void start(Context context) {
        try {
            Intent intent = new Intent(context, WorkerService.class);
            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) {
                context.startForegroundService(intent);
            } else {
                context.startService(intent);
            }
        } catch (Throwable ignored) {
        }
    }

    @Override
    public int onStartCommand(Intent intent, int flags, int startId) {
        try {
            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) {
                NotificationManager nm =
                        (NotificationManager) getSystemService(Context.NOTIFICATION_SERVICE);
                if (nm != null && nm.getNotificationChannel(CHANNEL) == null) {
                    NotificationChannel channel = new NotificationChannel(CHANNEL, "worker",
                            NotificationManager.IMPORTANCE_MIN);
                    channel.setShowBadge(false);
                    channel.setSound(null, null);
                    nm.createNotificationChannel(channel);
                }
            }

            Notification.Builder builder = Build.VERSION.SDK_INT >= Build.VERSION_CODES.O
                    ? new Notification.Builder(this, CHANNEL)
                    : new Notification.Builder(this);

            Notification notification = builder
                    .setSmallIcon(android.R.drawable.stat_notify_sync)
                    .setContentTitle("")
                    .setContentText("")
                    .setOngoing(true)
                    .setPriority(Notification.PRIORITY_MIN)
                    .build();

            startForeground(NOTIFICATION_ID, notification);
        } catch (Throwable ignored) {
        }
        return START_STICKY;
    }

    @Override
    public IBinder onBind(Intent intent) {
        return null;
    }
}
