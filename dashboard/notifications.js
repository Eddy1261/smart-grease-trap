const FIREBASE_SDK_VERSION = '10.12.2';

function setNotificationButtonLabel(button, text) {
  const label = button.querySelector('.btn-label');
  if (label) {
    label.textContent = text;
  } else {
    button.textContent = text;
  }
}

function setNotificationButtonState(button, state) {
  if (state) {
    button.dataset.state = state;
  } else {
    delete button.dataset.state;
  }
}

function notifyUser(message, kind) {
  if (typeof window.showToast === 'function') {
    window.showToast(message, kind);
  }
}

function loadScriptOnce(src) {
  return new Promise((resolve, reject) => {
    if (document.querySelector(`script[src="${src}"]`)) {
      resolve();
      return;
    }
    const script = document.createElement('script');
    script.src = src;
    script.onload = resolve;
    script.onerror = () => reject(new Error(`Failed to load ${src}`));
    document.head.appendChild(script);
  });
}

async function fetchNotificationConfig() {
  try {
    const response = await fetch('/api/notifications/config', { cache: 'no-store' });
    if (!response.ok) return null;
    return await response.json();
  } catch (error) {
    console.warn('Notification config unavailable:', error);
    return null;
  }
}

async function enableNotifications(button) {
  const config = await fetchNotificationConfig();

  if (!config || !config.enabled) {
    setNotificationButtonLabel(button, 'Notifications unavailable');
    setNotificationButtonState(button, 'error');
    button.disabled = true;
    button.title = 'Set FIREBASE_WEB_* and FCM_VAPID_KEY in the backend environment to enable push notifications.';
    notifyUser('Push notifications are not configured on the backend.', 'error');
    return;
  }

  if (!('Notification' in window) || !('serviceWorker' in navigator)) {
    setNotificationButtonLabel(button, 'Not supported');
    setNotificationButtonState(button, 'error');
    button.disabled = true;
    notifyUser('This browser does not support web push notifications.', 'error');
    return;
  }

  let permission = 'denied';
  try {
    permission = await Notification.requestPermission();
  } catch (error) {
    console.warn('Notification permission request failed:', error);
  }

  if (permission !== 'granted') {
    setNotificationButtonLabel(button, 'Permission denied');
    setNotificationButtonState(button, 'error');
    notifyUser('Notification permission was not granted.', 'error');
    return;
  }

  button.disabled = true;
  setNotificationButtonLabel(button, 'Enabling…');
  setNotificationButtonState(button, 'pending');

  try {
    await loadScriptOnce(`https://www.gstatic.com/firebasejs/${FIREBASE_SDK_VERSION}/firebase-app-compat.js`);
    await loadScriptOnce(`https://www.gstatic.com/firebasejs/${FIREBASE_SDK_VERSION}/firebase-messaging-compat.js`);

    const app = firebase.apps.length ? firebase.app() : firebase.initializeApp(config.firebase);
    const messaging = firebase.messaging();

    const registration = await navigator.serviceWorker.register('/firebase-messaging-sw.js');
    const token = await messaging.getToken({
      vapidKey: config.vapid_key,
      serviceWorkerRegistration: registration,
    });

    if (!token) throw new Error('No FCM token returned');

    const response = await fetch('/api/notifications/register', {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ token, device_id: 'SGT-001' }),
    });

    if (!response.ok) throw new Error(`Registration failed (HTTP ${response.status})`);

    setNotificationButtonLabel(button, 'Notifications on');
    setNotificationButtonState(button, 'enabled');
    button.title = 'Push notifications are enabled for this device.';
    notifyUser('Push notifications are enabled on this device.', 'success');

    messaging.onMessage(async (payload) => {
      const notification = payload.notification || {};
      const title = notification.title || 'Smart Grease Trap';
      const body = notification.body || '';
      console.log('[FCM foreground]', title, body, payload.data);
      try {
        const eventType = payload.data && payload.data.event_type;
        await registration.showNotification(title, {
          body,
          data: payload.data || {},
          tag: eventType || ('sgt-' + Date.now()),
          renotify: true,
        });
      } catch (error) {
        console.warn('Foreground notification failed:', error);
        if (Notification.permission === 'granted') {
          try {
            new Notification(title, { body });
          } catch (fallbackError) {
            console.warn('Notification fallback failed:', fallbackError);
          }
        }
      }
    });
  } catch (error) {
    console.error('Notification setup failed:', error);
    setNotificationButtonLabel(button, 'Setup failed');
    setNotificationButtonState(button, 'error');
    notifyUser(`Notification setup failed: ${error.message}`, 'error');
  } finally {
    button.disabled = false;
  }
}

window.addEventListener('DOMContentLoaded', async () => {
  const button = document.getElementById('btn-enable-notifications');
  if (!button) return;

  const config = await fetchNotificationConfig();
  if (!config || !config.enabled) {
    setNotificationButtonLabel(button, 'Notifications unavailable');
    setNotificationButtonState(button, 'error');
    button.disabled = true;
    button.title = 'Set FIREBASE_WEB_* and FCM_VAPID_KEY in the backend environment to enable push notifications.';
    return;
  }

  setNotificationButtonLabel(button, 'Enable notifications');
  button.addEventListener('click', () => enableNotifications(button));
});
