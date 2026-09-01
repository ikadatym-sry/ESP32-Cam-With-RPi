import os
import sys
import json
import time
import struct
import asyncio
import logging
from datetime import datetime
from collections import deque
from pathlib import Path
from typing import Optional, List, Dict, Any
from contextlib import asynccontextmanager

import aiohttp
import boto3
from botocore.config import Config as BotoConfig
from botocore.exceptions import ClientError
from fastapi import FastAPI, UploadFile, File, Form, HTTPException, BackgroundTasks, Request
from fastapi.responses import HTMLResponse, StreamingResponse, JSONResponse, PlainTextResponse
from fastapi.templating import Jinja2Templates
import uvicorn

# ==========================================
# 1. LOGGING & CONFIGURATION
# ==========================================
logging.basicConfig(
    level=logging.INFO,
    format="%(asctime)s [%(levelname)s] %(name)s: %(message)s"
)
logger = logging.getLogger("ESP32-Hub")

BASE_DIR = Path(__file__).resolve().parent
CONFIG_PATH = BASE_DIR / "config.json"

default_config = {
    "esp32_ip": "defaultIP",
    "esp32_stream_url": "http://defaultIP:8181/stream",
    "esp32_status_url": "http://defaultIP:8100/status",
    "esp32_logs_url": "http://defaultIP:8100/logs",
    "esp32_cmd_url": "http://defaultIP:8100/cmd",
    "esp32_ota_url": "http://defaultIP:8100/update",
    "esp32_ota_password": "admin",
    "r2_account_id": "",
    "r2_access_key_id": "",
    "r2_secret_access_key": "",
    "r2_bucket_name": "",
    "r2_video_prefix": "recordings/",
    "r2_log_prefix": "logs/",
    "max_storage_gb": 8.0,
    "max_log_storage_mb": 500.0,
    "log_auto_sync_hours": 6,
    "ram_buffer_dir": "/dev/shm/esp32cam_clips",
    "record_post_motion_sec": 8,
    "web_port": 8088
}

def load_config() -> Dict[str, Any]:
    if CONFIG_PATH.exists():
        try:
            with open(CONFIG_PATH, "r", encoding="utf-8") as f:
                return {**default_config, **json.load(f)}
        except Exception as e:
            logger.error(f"Error loading config.json: {e}")
    return default_config

config = load_config()

# Ensure RAM buffer directory exists (/dev/shm tmpfs = Zero SD Wear)
ram_dir = Path(config.get("ram_buffer_dir", "/dev/shm/esp32cam_clips"))
if not os.path.exists("/dev/shm"):
    ram_dir = BASE_DIR / "temp_ram_clips"
ram_dir.mkdir(parents=True, exist_ok=True)

# System Log in RAM
ram_log_file = ram_dir / "system_events.log"

def write_ram_log(msg: str):
    timestamp = datetime.now().strftime("%Y-%m-%d %H:%M:%S")
    line = f"[{timestamp}] {msg}\n"
    try:
        with open(ram_log_file, "a", encoding="utf-8") as f:
            f.write(line)
    except Exception:
        pass

# ==========================================
# 2. CLOUDFLARE R2 CLIENT SETUP
# ==========================================
def get_s2_client():
    account_id = config.get("r2_account_id")
    access_key = config.get("r2_access_key_id")
    secret_key = config.get("r2_secret_access_key")

    if not (account_id and access_key and secret_key):
        return None

    return boto3.client(
        "s3",
        endpoint_url=f"https://{account_id}.r2.cloudflarestorage.com",
        aws_access_key_id=access_key,
        aws_secret_access_key=secret_key,
        config=BotoConfig(signature_version="s3v4")
    )

# ==========================================
# 3. PURE PYTHON LIGHTWEIGHT AVI MJPEG WRITER
# ==========================================
class AviMjpegWriter:
    def __init__(self, filepath: Path, width: int = 800, height: int = 600, fps: int = 20):
        self.filepath = filepath
        self.width = width
        self.height = height
        self.fps = fps
        self.file = open(filepath, "wb")
        self.frame_count = 0
        self.index_entries = []
        self.movi_start = 0

        self.file.write(b"\x00" * 256)
        self.movi_start = self.file.tell()

    def add_frame(self, jpeg_data: bytes):
        if not jpeg_data:
            return
        offset = self.file.tell() - self.movi_start
        size = len(jpeg_data)
        pad = b"\x00" if (size % 2 != 0) else b""

        self.file.write(b"00dc")
        self.file.write(struct.pack("<I", size))
        self.file.write(jpeg_data)
        if pad:
            self.file.write(pad)

        self.index_entries.append((b"00dc", 0x10, offset, size))
        self.frame_count += 1

    def close(self):
        if self.frame_count == 0:
            self.file.close()
            try:
                os.remove(self.filepath)
            except OSError:
                pass
            return

        idx_pos = self.file.tell()
        self.file.write(b"idx1")
        self.file.write(struct.pack("<I", len(self.index_entries) * 16))
        for ckid, flags, offset, length in self.index_entries:
            self.file.write(ckid)
            self.file.write(struct.pack("<III", flags, offset, length))

        file_size = self.file.tell()
        movi_size = idx_pos - self.movi_start

        self.file.seek(0)
        self.file.write(b"RIFF")
        self.file.write(struct.pack("<I", file_size - 8))
        self.file.write(b"AVI ")

        hdrl_data = bytearray()
        us_per_frame = int(1000000 / self.fps)
        avih = struct.pack(
            "<IIIIIIIIIIIIII",
            us_per_frame, 0, 0, 0x10, self.frame_count, 0, 1, 1024*1024,
            self.width, self.height, 0, 0, 0, 0
        )
        hdrl_data += b"avih" + struct.pack("<I", len(avih)) + avih

        strl_data = bytearray()
        strh = struct.pack(
            "<4s4sIIIIIIIIIIIIHH",
            b"vids", b"MJPG", 0, 0, 0, 1, self.fps, 0, self.frame_count,
            1024*1024, 0, 0, 0, 0, self.width, self.height
        )
        strl_data += b"strh" + struct.pack("<I", len(strh)) + strh

        strf = struct.pack(
            "<IIIHH4sIIII",
            40, self.width, self.height, 1, 24, b"MJPG", self.width * self.height * 3, 0, 0, 0
        )
        strl_data += b"strf" + struct.pack("<I", len(strf)) + strf

        hdrl_data += b"LIST" + struct.pack("<I", len(strl_data) + 4) + b"strl" + strl_data
        self.file.write(b"LIST" + struct.pack("<I", len(hdrl_data) + 4) + b"hdrl" + hdrl_data)
        self.file.write(b"LIST" + struct.pack("<I", movi_size + 4) + b"movi")
        self.file.close()

# ==========================================
# 4. GLOBAL STATE
# ==========================================
class HubState:
    def __init__(self):
        self.latest_jpeg: Optional[bytes] = None
        self.latest_motion: bool = False
        self.latest_motion_score: int = 0
        self.last_frame_time: float = time.time()
        self.is_recording: bool = False
        self.stream_paused: bool = False
        self.esp_online: bool = False
        self.current_writer: Optional[AviMjpegWriter] = None
        self.current_clip_path: Optional[Path] = None
        self.last_motion_time: float = 0.0
        self.pre_buffer: deque = deque(maxlen=25)
        self.esp32_telemetry: Dict[str, Any] = {}
        self.esp32_logs: List[str] = []
        self.last_reset_reason: str = "Unknown"

hub_state = HubState()

# ==========================================
# 5. ASYNC TASKS: STREAMING, RECORDING, R2 SYNC
# ==========================================
async def upload_to_r2_and_prune(clip_path: Path):
    """Uploads recorded video clip to Cloudflare R2 & enforces 8GB limit"""
    try:
        s3 = get_s2_client()
        bucket = config.get("r2_bucket_name")
        if not s3 or not bucket:
            write_ram_log(f"R2 not configured. Clip saved in RAM: {clip_path.name}")
            return

        prefix = config.get("r2_video_prefix", "recordings/").strip("/")
        prefix_str = f"{prefix}/" if prefix else ""
        filename = clip_path.name
        key = f"{prefix_str}{filename}"

        write_ram_log(f"Uploading {filename} ({clip_path.stat().st_size / 1024:.1f} KB) to R2 ({key})...")
        with open(clip_path, "rb") as data:
            s3.put_object(Bucket=bucket, Key=key, Body=data, ContentType="video/x-msvideo")
        write_ram_log(f"Upload complete: {key}")

        if clip_path.exists():
            clip_path.unlink()

        max_bytes = int(config.get("max_storage_gb", 8.0) * 1024 * 1024 * 1024)
        response = s3.list_objects_v2(Bucket=bucket, Prefix=prefix_str)
        objects = response.get("Contents", [])

        total_bytes = sum(obj["Size"] for obj in objects)
        if total_bytes > max_bytes:
            sorted_objs = sorted(objects, key=lambda x: x["LastModified"])
            for old_obj in sorted_objs:
                write_ram_log(f"Video Quota > 8GB! Auto-pruning oldest clip: {old_obj['Key']}")
                s3.delete_object(Bucket=bucket, Key=old_obj["Key"])
                total_bytes -= old_obj["Size"]
                if total_bytes <= max_bytes * 0.95:
                    break

    except Exception as e:
        logger.error(f"Error during R2 video upload: {e}")
        write_ram_log(f"Video Upload ERROR: {e}")

async def archive_logs_to_r2():
    """Archives accumulated logs to Cloudflare R2 & enforces 500MB limit on log folder"""
    try:
        s3 = get_s2_client()
        bucket = config.get("r2_bucket_name")
        if not s3 or not bucket:
            return {"status": "error", "message": "Cloudflare R2 not configured"}

        prefix = config.get("r2_log_prefix", "logs/").strip("/")
        prefix_str = f"{prefix}/" if prefix else ""
        timestamp = datetime.now().strftime("%Y%m%d_%H%M%S")
        log_filename = f"log_{timestamp}.txt"
        log_key = f"{prefix_str}{log_filename}"
        temp_log_path = ram_dir / log_filename

        content = f"=== ESP32-CAM DIAGNOSTIC LOG ARCHIVE ===\n"
        content += f"Archived At: {datetime.now().isoformat()}\n"
        content += f"Last Boot Reason: {hub_state.last_reset_reason}\n\n"
        content += "--- ESP32 Hardware Logs ---\n"
        content += "\n".join(hub_state.esp32_logs) + "\n\n"
        content += "--- RPi 4 Event Logs ---\n"
        if ram_log_file.exists():
            with open(ram_log_file, "r", encoding="utf-8") as f:
                content += f.read()

        with open(temp_log_path, "w", encoding="utf-8") as f:
            f.write(content)

        with open(temp_log_path, "rb") as f_data:
            s3.put_object(Bucket=bucket, Key=log_key, Body=f_data, ContentType="text/plain")

        if temp_log_path.exists():
            temp_log_path.unlink()

        write_ram_log(f"Archived log snapshot to Cloudflare R2: {log_key}")

        max_log_bytes = int(config.get("max_log_storage_mb", 500.0) * 1024 * 1024)
        response = s3.list_objects_v2(Bucket=bucket, Prefix=prefix_str)
        log_objects = response.get("Contents", [])

        total_log_bytes = sum(obj["Size"] for obj in log_objects)
        if total_log_bytes > max_log_bytes:
            sorted_logs = sorted(log_objects, key=lambda x: x["LastModified"])
            for old_log in sorted_logs:
                write_ram_log(f"Log Quota > 500MB! Auto-pruning oldest log: {old_log['Key']}")
                s3.delete_object(Bucket=bucket, Key=old_log["Key"])
                total_log_bytes -= old_log["Size"]
                if total_log_bytes <= max_log_bytes * 0.95:
                    break

        return {"status": "success", "key": log_key, "size": len(content.encode('utf-8'))}

    except Exception as e:
        logger.error(f"Error archiving logs to R2: {e}")
        return {"status": "error", "message": str(e)}

async def periodic_log_archiver_loop():
    while True:
        hours = config.get("log_auto_sync_hours", 6)
        await asyncio.sleep(hours * 3600)
        await archive_logs_to_r2()

async def stream_ingest_loop():
    was_connected = False
    while True:
        if hub_state.stream_paused:
            await asyncio.sleep(0.5)
            continue

        stream_url = config.get("esp32_stream_url")
        try:
            timeout = aiohttp.ClientTimeout(total=None, sock_connect=3, sock_read=8)
            async with aiohttp.ClientSession(timeout=timeout) as session:
                async with session.get(stream_url) as response:
                    if response.status != 200:
                        await asyncio.sleep(2)
                        continue

                    if not was_connected:
                        logger.info(f"Stream connection established with ESP32: {stream_url}")
                        write_ram_log(f"Stream connected: {stream_url}")
                        was_connected = True
                        hub_state.esp_online = True

                    buffer = bytearray()
                    async for chunk in response.content.iter_chunked(4096):
                        if hub_state.stream_paused:
                            break

                        buffer.extend(chunk)

                        while True:
                            soi = buffer.find(b"\xff\xd8")
                            eoi = buffer.find(b"\xff\xd9", soi + 2) if soi != -1 else -1

                            if soi != -1 and eoi != -1:
                                jpeg_bytes = bytes(buffer[soi : eoi + 2])
                                header_part = buffer[:soi].decode("latin-1", errors="ignore")
                                buffer = buffer[eoi + 2 :]

                                is_motion = "X-Motion: true" in header_part
                                score = 0
                                if "X-Motion-Score:" in header_part:
                                    try:
                                        score = int(header_part.split("X-Motion-Score:")[1].split("\r\n")[0].strip())
                                    except Exception:
                                        pass

                                hub_state.latest_jpeg = jpeg_bytes
                                hub_state.latest_motion = is_motion
                                hub_state.latest_motion_score = score
                                hub_state.last_frame_time = time.time()
                                hub_state.pre_buffer.append(jpeg_bytes)

                                if is_motion:
                                    hub_state.last_motion_time = time.time()
                                    if not hub_state.is_recording:
                                        timestamp = datetime.now().strftime("%Y%m%d_%H%M%S")
                                        clip_file = ram_dir / f"motion_{timestamp}.avi"
                                        hub_state.current_writer = AviMjpegWriter(clip_file, width=800, height=600, fps=20)
                                        hub_state.current_clip_path = clip_file
                                        hub_state.is_recording = True
                                        write_ram_log(f"Motion trigger active! Recording to RAM: {clip_file.name}")

                                        for pre_frame in hub_state.pre_buffer:
                                            hub_state.current_writer.add_frame(pre_frame)

                                if hub_state.is_recording:
                                    hub_state.current_writer.add_frame(jpeg_bytes)
                                    post_sec = config.get("record_post_motion_sec", 8)
                                    if (time.time() - hub_state.last_motion_time) > post_sec:
                                        hub_state.current_writer.close()
                                        clip_to_upload = hub_state.current_clip_path
                                        hub_state.is_recording = False
                                        hub_state.current_writer = None
                                        hub_state.current_clip_path = None
                                        asyncio.create_task(upload_to_r2_and_prune(clip_to_upload))
                            else:
                                break

        except Exception as e:
            if was_connected:
                logger.info("ESP32 disconnected / power cut. Waiting for device to power back on...")
                write_ram_log("ESP32 power disconnected. Entering standby reconnect loop...")
                was_connected = False
                hub_state.esp_online = False
                hub_state.latest_jpeg = None

            if hub_state.is_recording and hub_state.current_writer:
                write_ram_log("Stream dropped mid-recording. Finalizing partial clip...")
                hub_state.current_writer.close()
                clip_to_upload = hub_state.current_clip_path
                hub_state.is_recording = False
                hub_state.current_writer = None
                hub_state.current_clip_path = None
                if clip_to_upload and clip_to_upload.exists():
                    asyncio.create_task(upload_to_r2_and_prune(clip_to_upload))

            await asyncio.sleep(2)

async def telemetry_and_log_poll_loop():
    while True:
        status_url = config.get("esp32_status_url")
        logs_url = config.get("esp32_logs_url")

        try:
            timeout = aiohttp.ClientTimeout(total=2)
            async with aiohttp.ClientSession(timeout=timeout) as session:
                async with session.get(status_url) as resp:
                    if resp.status == 200:
                        text_resp = await resp.text()
                        try:
                            data = json.loads(text_resp)
                            hub_state.esp32_telemetry = data
                            hub_state.esp_online = True
                            new_reason = data.get("reset_reason", "Unknown")
                            if new_reason != hub_state.last_reset_reason and new_reason != "Unknown":
                                hub_state.last_reset_reason = new_reason
                                write_ram_log(f"ESP32 Boot Reported: {new_reason}")
                        except Exception:
                            pass
                    else:
                        hub_state.esp_online = False
        except Exception:
            hub_state.esp_online = False

        try:
            timeout = aiohttp.ClientTimeout(total=2)
            async with aiohttp.ClientSession(timeout=timeout) as session:
                async with session.get(logs_url) as resp:
                    if resp.status == 200:
                        text_resp = await resp.text()
                        try:
                            data = json.loads(text_resp)
                            hub_state.esp32_logs = data.get("logs", [])
                        except Exception:
                            pass
        except Exception:
            pass

        await asyncio.sleep(1.5)

# ==========================================
# 6. FASTAPI ROUTES & LIFESPAN
# ==========================================
@asynccontextmanager
async def lifespan(app: FastAPI):
    write_ram_log("ESP32-CAM Hub Service Started on RPi 4.")
    t1 = asyncio.create_task(stream_ingest_loop())
    t2 = asyncio.create_task(telemetry_and_log_poll_loop())
    t3 = asyncio.create_task(periodic_log_archiver_loop())
    yield
    t1.cancel()
    t2.cancel()
    t3.cancel()

app = FastAPI(title="ESP32-CAM RPi 4 Hub", lifespan=lifespan)
templates = Jinja2Templates(directory=str(BASE_DIR / "templates"))

@app.get("/", response_class=HTMLResponse)
async def serve_index(request: Request):
    return templates.TemplateResponse("index.html", {"request": request})

@app.get("/video_feed")
async def video_feed():
    async def frame_generator():
        while True:
            if not hub_state.stream_paused and hub_state.latest_jpeg:
                yield (
                    b"--frame\r\n"
                    b"Content-Type: image/jpeg\r\n\r\n" + hub_state.latest_jpeg + b"\r\n"
                )
            await asyncio.sleep(0.04)

    return StreamingResponse(frame_generator(), media_type="multipart/x-mixed-replace; boundary=frame")

@app.post("/api/stream/toggle")
async def toggle_stream():
    hub_state.stream_paused = not hub_state.stream_paused
    state_str = "PAUSED" if hub_state.stream_paused else "RESUMED"
    write_ram_log(f"Video Stream {state_str} by user command.")
    return {"stream_paused": hub_state.stream_paused, "status": state_str}

@app.get("/api/status")
async def get_status():
    tel = hub_state.esp32_telemetry.copy()
    tel["esp32_ip"] = config.get("esp32_ip")
    tel["r2_bucket"] = config.get("r2_bucket_name")
    tel["is_recording"] = hub_state.is_recording
    tel["stream_paused"] = hub_state.stream_paused
    tel["esp_online"] = hub_state.esp_online
    tel["motion_detected"] = hub_state.latest_motion or tel.get("motion_detected", False)
    tel["motion_score"] = hub_state.latest_motion_score or tel.get("motion_score", 0)
    
    if not hub_state.esp_online or "temperature_c" not in tel:
        tel["temperature_c"] = 0.0
        tel["psram_total_bytes"] = 2097152
        tel["psram_free_bytes"] = 0
        tel["psram_min_free_bytes"] = 0
        tel["heap_free_bytes"] = 0
        tel["heap_min_free_bytes"] = 0
        tel["wifi_rssi_dbm"] = -99
        tel["fps"] = 0.0
        tel["uptime_seconds"] = 0
        tel["reset_reason"] = "Offline / Waiting for ESP32"

    return JSONResponse(tel)

@app.get("/api/logs")
async def get_logs():
    rpi_logs = []
    if ram_log_file.exists():
        try:
            with open(ram_log_file, "r", encoding="utf-8") as f:
                rpi_logs = f.readlines()[-60:]
        except Exception:
            pass

    return {
        "reset_reason": hub_state.last_reset_reason,
        "esp32_logs": hub_state.esp32_logs,
        "rpi_logs": [line.strip() for line in rpi_logs]
    }

@app.get("/api/logs/download")
async def download_logs():
    content = f"=== ESP32-CAM DIAGNOSTIC LOGS ===\n"
    content += f"Export Time: {datetime.now().isoformat()}\n"
    content += f"Last Boot Reason: {hub_state.last_reset_reason}\n\n"
    content += "--- ESP32 Serial Logs ---\n"
    content += "\n".join(hub_state.esp32_logs) + "\n\n"
    content += "--- RPi 4 Hub Events ---\n"
    if ram_log_file.exists():
        with open(ram_log_file, "r", encoding="utf-8") as f:
            content += f.read()

    return PlainTextResponse(
        content,
        headers={"Content-Disposition": "attachment; filename=esp32cam_diagnostics.txt"}
    )

@app.post("/api/logs/sync_r2")
async def sync_logs_to_r2_endpoint():
    result = await archive_logs_to_r2()
    return JSONResponse(result)

@app.get("/api/r2_logs")
async def list_r2_logs():
    s3 = get_s2_client()
    bucket = config.get("r2_bucket_name")
    prefix = config.get("r2_log_prefix", "logs/").strip("/")
    prefix_str = f"{prefix}/" if prefix else ""

    if not s3 or not bucket:
        return {"logs": [], "total_log_bytes": 0, "max_log_storage_mb": config.get("max_log_storage_mb", 500.0)}

    try:
        response = s3.list_objects_v2(Bucket=bucket, Prefix=prefix_str)
        objects = response.get("Contents", [])
        sorted_objs = sorted(objects, key=lambda x: x["LastModified"], reverse=True)
        total_log_bytes = sum(obj["Size"] for obj in sorted_objs)

        logs = []
        for obj in sorted_objs:
            key = obj["Key"]
            if key == prefix_str:
                continue
            presigned_url = s3.generate_presigned_url(
                "get_object",
                Params={"Bucket": bucket, "Key": key},
                ExpiresIn=3600
            )
            display_name = key.replace(prefix_str, "")
            logs.append({
                "key": display_name,
                "full_key": key,
                "size": obj["Size"],
                "last_modified": obj["LastModified"].isoformat(),
                "url": presigned_url
            })

        return {
            "logs": logs,
            "total_log_bytes": total_log_bytes,
            "max_log_storage_mb": config.get("max_log_storage_mb", 500.0)
        }
    except Exception as e:
        return {"logs": [], "total_log_bytes": 0, "max_log_storage_mb": 500.0, "error": str(e)}

@app.delete("/api/r2_logs/{key:path}")
async def delete_r2_log(key: str):
    s3 = get_s2_client()
    bucket = config.get("r2_bucket_name")
    prefix = config.get("r2_log_prefix", "logs/").strip("/")
    prefix_str = f"{prefix}/" if prefix else ""

    if not s3 or not bucket:
        raise HTTPException(status_code=400, detail="R2 not configured")

    try:
        full_key = key if key.startswith(prefix_str) else f"{prefix_str}{key}"
        s3.delete_object(Bucket=bucket, Key=full_key)
        return {"status": "success", "deleted": key}
    except Exception as e:
        raise HTTPException(status_code=500, detail=str(e))

@app.post("/api/cmd")
async def execute_command(req: Request):
    cmd_url = config.get("esp32_cmd_url")
    body = await req.json()
    action = body.get("action")

    write_ram_log(f"Command requested: {action} (args: {body}) -> {cmd_url}")

    try:
        timeout = aiohttp.ClientTimeout(total=5)
        async with aiohttp.ClientSession(timeout=timeout) as session:
            async with session.get(cmd_url, params=body) as resp:
                text_resp = await resp.text()
                try:
                    data = json.loads(text_resp)
                except Exception:
                    data = {"status": "ok" if resp.status == 200 else "error", "message": text_resp}
                return JSONResponse(data, status_code=resp.status)
    except Exception as e:
        err_msg = f"{type(e).__name__}: {e}"
        logger.error(f"Command execution error on {cmd_url}: {err_msg}")
        write_ram_log(f"Command execution error: {err_msg}")
        return JSONResponse({"status": "error", "message": err_msg}, status_code=500)

@app.get("/api/recordings")
async def list_recordings():
    s3 = get_s2_client()
    bucket = config.get("r2_bucket_name")
    prefix = config.get("r2_video_prefix", "recordings/").strip("/")
    prefix_str = f"{prefix}/" if prefix else ""

    if not s3 or not bucket:
        return {"recordings": [], "total_bytes": 0, "max_storage_gb": config.get("max_storage_gb", 8.0)}

    try:
        response = s3.list_objects_v2(Bucket=bucket, Prefix=prefix_str)
        objects = response.get("Contents", [])
        sorted_objs = sorted(objects, key=lambda x: x["LastModified"], reverse=True)
        total_bytes = sum(obj["Size"] for obj in sorted_objs)

        recordings = []
        for obj in sorted_objs:
            key = obj["Key"]
            if key == prefix_str:
                continue
            presigned_url = s3.generate_presigned_url(
                "get_object",
                Params={"Bucket": bucket, "Key": key},
                ExpiresIn=3600
            )
            display_name = key.replace(prefix_str, "")
            recordings.append({
                "key": display_name,
                "full_key": key,
                "size": obj["Size"],
                "last_modified": obj["LastModified"].isoformat(),
                "url": presigned_url
            })

        return {
            "recordings": recordings,
            "total_bytes": total_bytes,
            "max_storage_gb": config.get("max_storage_gb", 8.0)
        }
    except Exception as e:
        return {"recordings": [], "total_bytes": 0, "max_storage_gb": 8.0, "error": str(e)}

@app.delete("/api/recordings/{key:path}")
async def delete_recording(key: str):
    s3 = get_s2_client()
    bucket = config.get("r2_bucket_name")
    prefix = config.get("r2_video_prefix", "recordings/").strip("/")
    prefix_str = f"{prefix}/" if prefix else ""

    if not s3 or not bucket:
        raise HTTPException(status_code=400, detail="R2 not configured")

    try:
        full_key = key if key.startswith(prefix_str) else f"{prefix_str}{key}"
        s3.delete_object(Bucket=bucket, Key=full_key)
        return {"status": "success", "deleted": key}
    except Exception as e:
        raise HTTPException(status_code=500, detail=str(e))

@app.post("/api/ota_upload")
async def ota_upload(update: UploadFile = File(...), password: Optional[str] = Form(None)):
    ota_base_url = config.get("esp32_ota_url")
    ota_pwd = password or config.get("esp32_ota_password", "admin")
    ota_url = f"{ota_base_url}?pwd={ota_pwd}"
    write_ram_log(f"Forwarding OTA firmware binary ({update.filename}) to ESP32 with auth: {ota_url}")

    try:
        file_bytes = await update.read()
        timeout = aiohttp.ClientTimeout(total=60)
        async with aiohttp.ClientSession(timeout=timeout) as session:
            async with session.post(ota_url, data=file_bytes, headers={"Content-Type": "application/octet-stream"}) as resp:
                resp_text = await resp.text()
                if resp.status == 200:
                    return {"status": "success", "message": resp_text}
                else:
                    raise HTTPException(status_code=resp.status, detail=f"ESP32 OTA failed: {resp_text}")
    except Exception as e:
        raise HTTPException(status_code=500, detail=str(e))

if __name__ == "__main__":
    port = int(config.get("web_port", 8088))
    logger.info(f"Starting ESP32-CAM Hub on port {port}...")
    uvicorn.run("app:app", host="0.0.0.0", port=port, reload=False)
