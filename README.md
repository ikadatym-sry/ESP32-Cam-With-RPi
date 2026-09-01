# Features:
- Cloudflare R2 upload support
- Webserver (Designed by Gemini 3.7 Flash I believed if it's designed by me, it will worse than the 90's websites)
- ESP32-Cam status such as: Realtime Temperature, PSRAM available, WiFi status, etc.
- No SD Card needed to record the video!
- It should use only RPi's RAM to keep and sent the files to R2
  
# Tested on
ESP32-Cam 2MB PSRAM Unknown model name from Shopee + Raspberry Pi 4 

# -- Please read --
- I may or may not update this repo. Depends on my mood
- Since 80% of this repo created by Gemini 3.7 Flash because I'm very lazy, I think it's freely to clone or fork or edit the code without ask any permission from me.
- I don't do "All in One ESP32-Cam Webserver" because Cloudflare R2 support needs more RAM, since my ESP32-Cam has only 2MB of PSRAM, that's impossible to do that(for me).
- To access the livestream, you have to enter `http://TheRPiIP:8088`
- It should use maximum 8.5GB of R2 storage as I designed (8GB for recording and 500MB for log)
