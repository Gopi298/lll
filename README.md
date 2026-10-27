# Smart Light - Online Control (Vercel + Firebase + ESP8266)

```
Phone / PC  -->  Vercel website (login priya / priya@143)  -->  Firebase Realtime Database  <--  ESP8266 board
```

Your Firebase project (`app-registration-2f6cb`) is already filled in, in both the website code and the Arduino code.
No database secret is needed. The board and the website log in to Firebase with the project API key (anonymous sign-in).

## Step 1 - Firebase (2 settings, one time)

1. https://console.firebase.google.com -> project **app-registration-2f6cb**.
2. **Build -> Authentication -> Get started -> Sign-in method -> Anonymous -> Enable -> Save.**
3. **Build -> Realtime Database -> Rules**, replace everything with this and press **Publish**:

```
{
  "rules": {
    "light": {
      ".read": "auth != null",
      ".write": "auth != null"
    }
  }
}
```

## Step 2 - Upload the ESP8266 code

1. Arduino IDE -> Library Manager -> install **ArduinoJson** (Benoit Blanchon).
2. Open `esp8266_light_online.ino`, set `WIFI_SSID` and `WIFI_PASSWORD`. Upload.
3. Serial Monitor (115200) must show: `Firebase login OK`.
   The local dashboard at the board's IP still works with `priya` / `priya@143`.

## Step 3 - Deploy the website on Vercel

1. Put this folder on GitHub, then Vercel -> **Add New -> Project** -> import it. Framework preset: **Other**. Deploy.
2. Optional but recommended: Vercel -> Settings -> Environment Variables -> add `SESSION_SECRET` = any long random text, then redeploy.
3. Open your Vercel link and log in with `priya` / `priya@143`.

To change the website login later, set `LOGIN_USER` and `LOGIN_PASS` in Vercel (and the same values in the Arduino file for the local page).

## Using it

The badge shows **Board online** when the board has contacted Firebase in the last 45 seconds.
A command reaches the light in about 3 seconds. Schedules saved on the website or on the local IP page stay in sync.

## Problems?

| Serial Monitor says | Fix |
|---|---|
| `Firebase login failed, HTTP 400` | Anonymous sign-in is not enabled (Step 1, point 2) |
| `Firebase says access denied` | Rules not published (Step 1, point 3) |
| Nothing about Firebase | WiFi name or password wrong |
