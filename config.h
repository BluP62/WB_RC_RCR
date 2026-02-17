

// ----- Webserver -----
#define WEBSERVER_PORT 80

// ----- URLs -----
// Please dajust to your IPs and Shelly commands
#define URL_ON    "http://10.0.0.5/cm?cmnd=Power%20On";
#define URL_OFF   "http://10.0.0.5/cm?cmnd=Power%20Off";
#define URL_PARAM "http://10.0.1.0/getParameters";

// EV SOC API (lokaler Webserver)
// Auskommentieren wenn kein lokaler EV-SOC-Server vorhanden
#define USE_EV_SOC_API

#ifdef USE_EV_SOC_API
  #define EV_SOC_URL "http://pv-automat:5001/api/ev_soc"
#endif

// ----- Pins -----


