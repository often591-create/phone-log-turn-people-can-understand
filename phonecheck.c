/*
 * 手機健檢：分析 Android bugreport
 * 支援：小米、三星、OPPO（含 OnePlus、realme）、vivo（含 iQOO），其他 Android 手機會讀共通的部分
 * 程式會自動認出是哪一家的手機，不用自己選
 * 用 Dev-C++ 編譯，檔案請存成 .c
 *
 * 檔案的順序：
 *   1. 共用工具和資料
 *   2. Android 內建的資料（每家手機都有）
 *   3. 小米   4. 三星   5. OPPO   6. vivo
 *   7. 主程式（選檔案、讀檔、判斷、各分頁、主選單）
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <dirent.h>
#include <sys/stat.h>
#include <time.h>

#define LINE_SIZE 65536
#define MAX_LIST  50
#ifdef _WIN32
#define SEP "\\"          /* Windows 的資料夾分隔符號 */
#else
#define SEP "/"           /* 手機（Android）的資料夾分隔符號 */
#endif

/* 讀檔用的一行（很長，所以放在外面） */
static char line[LINE_SIZE];

/* ================= 共用小工具 ================= */

/* 等使用者按 Enter */
void wait_enter(void)
{
    int c;
    while ((c = getchar()) != '\n' && c != EOF) { }
    printf("\n 按 Enter 回主選單");
    while ((c = getchar()) != '\n' && c != EOF) { }
}

/* s 是不是以 p 開頭 */
int starts_with(const char *s, const char *p)
{
    return strncmp(s, p, strlen(p)) == 0;
}

/* 去掉字串尾巴的換行和空白 */
void trim(char *s)
{
    int n = strlen(s);
    while (n > 0 && (s[n-1] == '\n' || s[n-1] == '\r' || s[n-1] == ' ' || s[n-1] == '\t')) {
        s[n-1] = '\0';
        n--;
    }
}

/* 從 src 複製到 out，遇到 stop 字元就停 */
void copy_until(const char *src, char stop, char *out, int size)
{
    int i = 0;
    while (src[i] != '\0' && src[i] != stop && src[i] != '\n' && src[i] != '\r' && i < size - 1) {
        out[i] = src[i];
        i++;
    }
    out[i] = '\0';
}

/* 系統參數 [key]: [value]，找到就把 value 放進 out */
int get_prop(const char *s, const char *key, char *out, int size)
{
    char pattern[128];
    sprintf(pattern, "[%s]: [", key);
    if (starts_with(s, pattern)) {
        copy_until(s + strlen(pattern), ']', out, size);
        return 1;
    }
    return 0;
}

/* 這行是不是 log 格式（開頭像 11-18 05:28:49） */
int is_log_line(const char *s)
{
    return isdigit((unsigned char)s[0]) && isdigit((unsigned char)s[1]) && s[2] == '-' &&
           isdigit((unsigned char)s[3]) && isdigit((unsigned char)s[4]) && s[5] == ' ';
}

/* 日期換成天數，用來算兩個日期差幾天 */
long days_of(int y, int m, int d)
{
    long era, yoe, doy, doe;
    if (m <= 2) y--;
    era = y / 400;
    yoe = y - era * 400;
    doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + doe;
}

/* 2026-11-16T20:30:13.68 換成秒數 */
double iso_to_sec(const char *s)
{
    int y, mo, d, h, mi;
    double sec;
    if (sscanf(s, "%d-%d-%dT%d:%d:%lf", &y, &mo, &d, &h, &mi, &sec) != 6) return -1;
    return days_of(y, mo, d) * 86400.0 + h * 3600.0 + mi * 60.0 + sec;
}

/* log 的時間 11-20 20:27:53.824 換成秒數（年份要另外給） */
double log_to_sec(const char *s, int year)
{
    int mo, d, h, mi;
    double sec;
    if (sscanf(s, "%d-%d %d:%d:%lf", &mo, &d, &h, &mi, &sec) != 5) return -1;
    return days_of(year, mo, d) * 86400.0 + h * 3600.0 + mi * 60.0 + sec;
}

/* 秒數換成「X 小時 Y 分」 */
void sec_to_text(double sec, char *out)
{
    long s = (long)sec;
    if (s >= 3600) sprintf(out, "%ld 小時 %ld 分", s / 3600, (s % 3600) / 60);
    else           sprintf(out, "%ld 分", s / 60);
}

/* 系統看到的容量（GB）換成包裝上寫的容量，例如 10.9 → 12、223 → 256 */
int round_size(double gb, int sizes[], int n)
{
    int i;
    for (i = 0; i < n; i++) {
        if (gb <= sizes[i]) return sizes[i];
    }
    return (int)(gb + 0.5);
}
int ram_sizes[] = {2, 3, 4, 6, 8, 12, 16, 24, 32};
int rom_sizes[] = {16, 32, 64, 128, 256, 512, 1024, 2048};

/* ================= 對照表 ================= */

/* 記憶體廠商碼（LPDDR MR5） */
int dram_code[] = {1, 5, 6, 8, 255};
const char *dram_name[] = {"三星 Samsung", "南亞 Nanya", "SK hynix", "華邦 Winbond", "美光 Micron"};
int dram_n = 5;

/* 儲存廠商碼（UFS） */
int ufs_code[] = {0x12C, 0x145, 0x198, 0x1AD, 0x1CE};
const char *ufs_name[] = {"美光 Micron", "Western Digital", "鎧俠 Kioxia", "SK hynix", "三星 Samsung"};
int ufs_n = 5;

/* 在對照表裡找代碼，找不到回傳 NULL */
const char *lookup(int code, int codes[], const char *names[], int n)
{
    int i;
    for (i = 0; i < n; i++) {
        if (codes[i] == code) return names[i];
    }
    return NULL;
}

/* 鏡頭感光元件型號 → 廠商 */
const char *camera_maker(const char *name)
{
    if (starts_with(name, "OV")) return "OmniVision";
    if (starts_with(name, "IMX") || starts_with(name, "LYT")) return "Sony";
    if (starts_with(name, "S5K") || starts_with(name, "JN") || starts_with(name, "HP")) return "Samsung";
    if (starts_with(name, "HI")) return "SK hynix";
    if (starts_with(name, "GC")) return "格科 GalaxyCore";
    if (starts_with(name, "SC")) return "思特威 SmartSens";
    return "";
}

/* 高通處理器代號 → 名稱 */
const char *soc_name(const char *soc)
{
    if (strcmp(soc, "SM8450") == 0) return "Snapdragon 8 Gen 1";
    if (strcmp(soc, "SM8475") == 0) return "Snapdragon 8+ Gen 1";
    if (strcmp(soc, "SM8550") == 0) return "Snapdragon 8 Gen 2";
    if (strcmp(soc, "SM8650") == 0) return "Snapdragon 8 Gen 3";
    if (strcmp(soc, "SM8750") == 0) return "Snapdragon 8 Elite";
    return "";
}

/* 錯誤訊息的黑名單（已知的系統雜訊標籤） */
const char *blacklist[] = {
    /* 小米 */
    "libunwindstack", "XQos", "libsensor-displayalgo", "NotificationVibratorHelper",
    "libsensor-frameBufferManager", "TransitionChain", "JobScheduler.JobStatus", "TEEC",
    "HfManager", "HfLooper",
    /* 三星 */
    "BufferQueueProducer", "HeatmapThread", "ProcessInfo", "HBD", "CamX", "CHIUSECASE",
    "QC2Interface", "minksocket", "RILD", "RILD2", "RILJ",
    /* OPPO */
    "SettingsState", "BinderStaticExtImpl", "KernelCpuUidUserSysTimeReader", "ShadowUtils",
    /* vivo */
    "android.hardware.wifi-service", "spcomlib", "PERFHAL-SSLITE", "PERFHAL-REQUEST", "AppOps",
    "ConversionHelperAidl", "_V_DisplayPowerControllerImpl", "WifiVcdRoaming"
};
int blacklist_n = 33;

/* App 被關的原因：關鍵字 → 中文。routine=1 表示常規，不列出；bad=1 表示異常 */
const char *kill_key[] = {
    "umms_selfcheck", "umms_pressure", "umms_launch", "isolated not needed",
    "SwipeUpClean", "WifiCloudSync",
    "depends on provider", "from process:system", "crash", "anr",
    "excessive cpu", "wake", "remove task", "timeout", "empty", "stop",
    "oneclick", "Sync transaction while frozen",
    "low memory", "PRESTART WITHOUT START"
};
const char *kill_cn[] = {
    "小米定時清理閒置程式", "記憶體緊張，清掉背景程式", "開啟大型 App 前先清出空間", "臨時程序用完關閉",
    "你從多工畫面滑掉", "Wi-Fi 雲端同步結束",
    "它依賴的程式正在關閉，被連帶關掉", "系統主動停止", "App 閃退後被關閉", "App 卡死無回應後被關閉",
    "背景佔用處理器太多", "背景一直不讓手機休眠", "從最近任務中移除", "啟動太久逾時", "閒置太久的空程序", "被強制停止",
    "你按了一鍵清理", "凍結中收到同步要求，被系統關掉",
    "記憶體不足，清掉背景程式", "預先開好但你沒打開，系統收回"
};
int kill_routine[] = {1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 0, 0, 1, 0, 0, 0, 1, 0, 1, 1};
int kill_bad[]     = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 0, 0, 0, 0, 0, 0, 0, 0};
int kill_n = 20;

/* ================= 讀出來的資料（每個廠商都可以填） ================= */

/* 系統 */
char g_maker[32] = "";                       /* 廠商：Xiaomi、samsung ... */
char g_model[64] = "", g_model_code[32] = "", g_android[16] = "";
char g_os_name[16] = "", g_os_ver[64] = "";  /* 例如 HyperOS + OS4.0.3.0、One UI + 8.0 */
char g_fw[64] = "";                          /* 韌體版本（有的廠商才有） */
char g_warranty[8] = "";                     /* 三星 Knox */
char g_patch[16] = "", g_verified[16] = "", g_locked[8] = "", g_capture[32] = "";
int g_cap_year = 2026;
char g_uptime[64] = "", g_poweroff[64] = "";
int g_poweroff_ok = 1;
int g_adb = -1, g_adb_wifi = -1, g_dev = -1;
int g_patch_days = -1;

/* 處理器與螢幕 */
char g_soc[32] = "", g_soc_maker[32] = "";
int g_cores = 0;
long g_freq_min = 0, g_freq_max = 0, g_time_total = 0, g_time_min = 0;
int g_w = 0, g_h = 0;
double g_max_hz = 0;
int g_oled = 0;
char g_hz_setting[48] = "";                  /* 例如「120 Hz」或「自動調整」 */

/* 零件 */
int g_dram_code = -1, g_dram_gb = 0, g_dram_freq = 0, g_dram_ch = 0;
char g_dram_type[16] = "";
int g_ufs_code = -1, g_ufs_gb = 0, g_ufs_ver = -1;
char g_ufs_model[32] = "", g_ufs_gear[8] = "", g_ufs_maker[32] = "";
char g_panel_text[96] = "";                  /* 螢幕面板（各廠商自己填） */
char g_fp_text[96] = "";                     /* 指紋（各廠商自己填） */
char g_touch[32] = "";
char g_cams[8][16];
int g_ncam = 0;
char g_gpu[48] = "";
char g_chargers[4][32];
int g_ncharger = 0;
char g_amp[32] = "", g_wifi_chip[16] = "";
int g_wifi7 = 0;

/* 記憶體 */
long g_memtotal_kb = 0;
struct Kill { char time[16]; char proc[96]; char reason[160]; };
struct Kill kills[MAX_LIST];
int g_nkill = 0, g_kill_bad = 0;
char unknown_reason[MAX_LIST][160];
int g_nunknown = 0;

/* 儲存 */
int g_life = -1, g_preeol = -1;
long g_ufs_err = 0;
long g_data_total_kb = 0;

/* 電池 */
int g_soh = -1, g_asoc = -1, g_cycle = -1, g_sd_min = -1, g_sd_max = -1;
long g_cv = -1;
char g_cycle_note[64] = "";                  /* 次數是怎麼算出來的 */
char g_first_use[16] = "";

/* 網路 */
int rssi_hist[121];          /* -120 ~ 0 dBm */
int speed_hist[2001];        /* 0 ~ 2000 Mbps */
int g_wifi_n = 0, g_wifi_score_on = 0;
int g_wifi_linkrate = 0;     /* 1 = 沒有實測速度，只有連線速率 */
struct WifiEvent { char time[16]; char text[64]; };
struct WifiEvent wev[MAX_LIST];
int g_nwev = 0;
char g_sim[2][16] = {"", ""};

/* 穩定性 */
struct Problem { char time[16]; char kind[16]; char name[128]; char file[64]; };
struct Problem probs[MAX_LIST];
int g_nprob = 0, g_ncrash = 0, g_nnative = 0, g_nanr = 0, g_nsys = 0;
int g_wait_process = 0;
char g_crash_time[16] = "";
char g_last_anr_file[64] = "", g_last_anr_name[128] = "";
int g_wait_subject = 0;

/* 錯誤訊息 */
long g_e = 0, g_f = 0, g_black = 0, g_kept = 0;
FILE *imp = NULL;

/* 背景凍結：每個廠商的凍結紀錄都放進 gz[] */
const char *watch_pkg[] = {"com.google.android.gms", "jp.naver.line.android", "com.facebook.orca", "com.tencent.mm"};
const char *watch_cn[]  = {"Google Play 服務", "LINE", "Messenger", "微信"};
int watch_n = 4;
int watch_uid[4] = {-1, -1, -1, -1};
struct GzEvent { int uid; char type; char packet; double t; };    /* type: F 凍結、T 解凍、D 結束 */
struct GzEvent gz[40000];
int g_ngz = 0;
int g_pkg_wait = -1;
char g_freeze_note[128] = "";

/* 歷史紀錄 */
char g_base[1000] = "";      /* zip 解壓縮後的資料夾 */
int g_has_dir = 0;           /* 有沒有解壓縮後的資料夾 */
int g_has_hist = 0;          /* 有沒有任何歷史紀錄 */
struct Hist { char time[24]; char text[128]; };
struct Hist upd[MAX_LIST], rst[MAX_LIST], crashes[MAX_LIST], tomb[MAX_LIST];
int g_nupd = 0, g_nrst = 0, g_ncrashes = 0, g_ntomb = 0;
int g_real_bootfail = 0;     /* 真的開機失敗幾次 */
double sd_time[200];         /* 關機的時間（秒），用來判斷崩潰是不是關機造成的 */
int g_nsd = 0;

/* 總覽用：每一頁的狀態 */
#define CAT_N 12
const char *cat_name[CAT_N] = {"系統", "處理器與螢幕", "記憶體", "儲存空間", "電池", "網路", "背景凍結", "穩定性", "錯誤訊息", "歷史紀錄", "溫度", "安全檢查"};
int cat_warn[CAT_N];
char cat_note[CAT_N][128];

/* ================= 共用的小工具（第 2 組） ================= */

/* 列出資料夾裡以 prefix 開頭的檔名，照名字排序，回傳個數 */
int list_dir(const char *dir, const char *prefix, char names[][128], int max)
{
    DIR *d;
    struct dirent *e;
    int n = 0, i, j;
    char tmp[128];
    d = opendir(dir);
    if (d == NULL) return 0;
    while ((e = readdir(d)) != NULL && n < max) {
        if (starts_with(e->d_name, prefix)) {
            strncpy(names[n], e->d_name, 127);
            names[n][127] = '\0';
            n++;
        }
    }
    closedir(d);
    for (i = 0; i < n - 1; i++) {
        for (j = 0; j < n - 1 - i; j++) {
            if (strcmp(names[j], names[j+1]) > 0) {
                strcpy(tmp, names[j]); strcpy(names[j], names[j+1]); strcpy(names[j+1], tmp);
            }
        }
    }
    return n;
}

/* 把歷史紀錄照時間排序 */
void sort_hist(struct Hist h[], int n)
{
    int i, j;
    struct Hist tmp;
    for (i = 0; i < n - 1; i++) {
        for (j = 0; j < n - 1 - i; j++) {
            if (strcmp(h[j].time, h[j+1].time) > 0) { tmp = h[j]; h[j] = h[j+1]; h[j+1] = tmp; }
        }
    }
}

/* 加一筆歷史紀錄 */
void add_hist(struct Hist h[], int *n, const char *time, const char *text)
{
    if (*n >= MAX_LIST) return;
    strncpy(h[*n].time, time, 23); h[*n].time[23] = '\0';
    strncpy(h[*n].text, text, 127); h[*n].text[127] = '\0';
    (*n)++;
}

/* 加一筆穩定性問題 */
void add_problem(const char *time, const char *kind, const char *name)
{
    if (g_nprob >= MAX_LIST) return;
    strcpy(probs[g_nprob].time, time);
    strcpy(probs[g_nprob].kind, kind);
    strncpy(probs[g_nprob].name, name, 127);
    probs[g_nprob].name[127] = '\0';
    probs[g_nprob].file[0] = '\0';
    g_nprob++;
}

/* 開檔讀 FS 資料夾裡的檔案，例如 open_fs("mnt" SEP "rescue" SEP "xxx") */
FILE *open_fs(const char *sub)
{
    char path[1300];
    sprintf(path, "%s" SEP "FS" SEP "%s", g_base, sub);
    return fopen(path, "rb");
}

/* ##################################################################
 * 2. Android 內建的資料
 * ################################################################## */

/*
 * Android 內建、每家手機都有的資料（開機原因紀錄、電池循環次數、電池原廠容量）
 * 和廠商一樣有 4 個函式，但 android_finish 要放在所有廠商的後面，
 * 這樣廠商自己讀到的資料會優先，讀不到才用 Android 內建的。
 */

int and_cycle = -1;
long and_design_mah = 0, and_full_mah = 0;
long and_real_fcc_mah = 0;            /* 廠商可以填「真正的滿電容量」，例如 OPPO */
char and_last_reason[64] = "";

/* 處理器各頻率的時間（算休息時間用） */
long cf_f[64], cf_t[64];
int cf_n = 0;

/* 開機原因紀錄 */
struct Hist boots[MAX_LIST];
int g_nboots = 0;
int and_boot_wait = 0;                /* 開機紀錄有好幾行，讀到第一行後要繼續讀 */

/* 開機原因 → 中文。abnormal 會回傳 1 */
int boot_reason_text(const char *r, char *out)
{
    /* r 例如 reboot,userrequested 或 cold,powerkey */
    const char *why = strchr(r, ',');
    why = (why != NULL) ? why + 1 : r;

    if (strstr(r, "panic") || strstr(r, "watchdog") || strstr(r, "hw_reset") || strstr(r, "wdt") ||
        strstr(r, "abnormal") || strstr(r, "kernel") || strstr(r, "thermal") || strstr(r, "crash")) {
        sprintf(out, "異常重開機 (%.40s)", r);
        return 1;
    }
    if (strcmp(why, "userrequested") == 0) strcpy(out, "使用者重新開機或關機");
    else if (strcmp(why, "ota") == 0) strcpy(out, "系統更新後重新開機");
    else if (strcmp(why, "factory_reset") == 0) strcpy(out, "恢復原廠");
    else if (strcmp(why, "lpm") == 0) strcpy(out, "關機充電後開機");
    else if (strcmp(why, "powerkey") == 0) strcpy(out, "按電源鍵開機");
    else if (strcmp(why, "recovery") == 0) strcpy(out, "進入 recovery");
    else if (strcmp(why, "silent") == 0) strcpy(out, "系統自動重開機（螢幕不亮）");
    else if (strcmp(r, "reboot") == 0) strcpy(out, "重新開機（沒有記錄原因）");
    else sprintf(out, "%.60s", r);
    return 0;
}

/* 開機紀錄的一筆：reboot,userrequested,1795175311（最後是時間，從 1970 年起算的秒數） */
void and_add_boot(const char *s)
{
    char r[64], text[128], when[24];
    const char *last = strrchr(s, ',');
    time_t t;
    int len;

    if (last == NULL || !isdigit((unsigned char)last[1])) {
        /* 沒有時間的，例如 reboot,1795002659 其實時間就在最後；只有原因沒時間的就跳過 */
        return;
    }
    len = last - s;
    if (len > 63) len = 63;
    strncpy(r, s, len); r[len] = '\0';
    t = (time_t)atol(last + 1);
    strftime(when, sizeof(when), "%Y-%m-%d %H:%M", localtime(&t));
    /* 1 月 1 日的時間通常是手機剛恢復原廠、時間還沒設定 */
    if (strstr(when, "-01-01 ") != NULL) strcat(when, "?");

    if (boot_reason_text(r, text)) add_hist(crashes, &g_ncrashes, when, text);
    add_hist(boots, &g_nboots, when, text);
    g_has_hist = 1;
}

/* ---------------- 讀檔時每一行 ---------------- */

void android_line(const char *section, const char *service)
{
    char *p;
    char tmp[200];

    /* 開機原因紀錄（跨好幾行） */
    if (and_boot_wait) {
        copy_until(line, ']', tmp, sizeof(tmp));
        trim(tmp);
        and_add_boot(tmp);
        if (strchr(line, ']') != NULL) and_boot_wait = 0;
        return;
    }
    if (line[0] == '[' && strcmp(section, "SYSTEM PROPERTIES") == 0) {
        get_prop(line, "sys.boot.reason.last", and_last_reason, sizeof(and_last_reason));
        if (starts_with(line, "[persist.sys.boot.reason.history]: [")) {
            copy_until(line + 36, ']', tmp, sizeof(tmp));
            trim(tmp);
            and_add_boot(tmp);
            if (strchr(line + 36, ']') == NULL) and_boot_wait = 1;
        }
        return;
    }

    /* 處理器各頻率的時間 */
    if (strcmp(section, "KERNEL CPUFREQ") == 0 && cf_n < 64 && sscanf(line, "%ld %ld", &cf_f[cf_n], &cf_t[cf_n]) == 2) cf_n++;

    /* 電池（Android 14 以後才有） */
    if ((p = strstr(line, "batteryCycleCount: ")) != NULL) and_cycle = atoi(p + 19);
    if ((p = strstr(line, "batteryFullChargeDesignCapacityUah: ")) != NULL) and_design_mah = atol(p + 36) / 1000;
    if ((p = strstr(line, "batteryFullChargeUah: ")) != NULL) and_full_mah = atol(p + 22) / 1000;
}

/* 卡死的 App 名稱如果是 Process ProcessRecord{cb6b5b8 9826:com.sina.weibo:remote/u0a309}，只留 com.sina.weibo:remote */
void and_clean_anr_names(void)
{
    int k;
    char *p, *q;
    for (k = 0; k < g_nprob; k++) {
        if ((p = strstr(probs[k].name, "ProcessRecord{")) != NULL && (p = strchr(p, ':')) != NULL) {
            q = strchr(p, '/');
            if (q != NULL) *q = '\0';
            memmove(probs[k].name, p + 1, strlen(p + 1) + 1);
        }
    }
}

/* ---------------- 讀完之後整理（要放在所有廠商後面） ---------------- */

void android_finish(void)
{
    long full;
    char text[128];
    int k;

    /* 處理器休息時間：最低頻率的 1.6 倍以內都算在休息
       （有的手機最低那一檔很少用，平常停在第二檔） */
    if (cf_n > 0 && g_freq_min > 0) {
        g_time_min = 0;
        for (k = 0; k < cf_n; k++) {
            if (cf_t[k] > 0 && cf_f[k] <= g_freq_min * 1.6) g_time_min += cf_t[k];
        }
    }

    and_clean_anr_names();

    /* 循環次數：廠商沒讀到才用這個（有的手機會寫 0，代表不支援） */
    if (g_cycle < 0 && and_cycle > 0) g_cycle = and_cycle;

    /* 健康度：滿電容量 ÷ 原廠容量 */
    full = (and_real_fcc_mah > 0) ? and_real_fcc_mah : and_full_mah;
    if (g_soh < 0 && and_design_mah > 0 && full > 0 && full < and_design_mah) {
        g_soh = (int)(full * 100 / and_design_mah);
        sprintf(g_cycle_note, "(健康度由目前滿電 %ld mAh ÷ 原廠 %ld mAh 推算)", full, and_design_mah);
    }

    /* 上次關機原因：廠商沒讀到才用這個 */
    if (g_poweroff[0] == '\0' && and_last_reason[0]) {
        g_poweroff_ok = !boot_reason_text(and_last_reason, text);
        strncpy(g_poweroff, text, sizeof(g_poweroff) - 1);
        g_poweroff[sizeof(g_poweroff) - 1] = '\0';
    }
}

/* ---------------- zip 裡的其他檔案 ---------------- */

void android_history(void)
{
    and_clean_anr_names();
}

/* ---------------- 歷史紀錄頁 ---------------- */

void android_history_page(void)
{
    int i, unsure = 0;
    if (g_nboots == 0) return;
    printf("\n ---- 最近幾次開機的原因（Android 內建紀錄）----\n");
    for (i = 0; i < g_nboots; i++) {
        printf(" %s  %s\n", boots[i].time, boots[i].text);
        if (strchr(boots[i].time, '?') != NULL) unsure = 1;
    }
    if (unsure) printf(" (時間後面有 ? 的，可能是手機時間還沒設定好)\n");
}

/* ##################################################################
 * 3. 小米
 * ################################################################## */

/*
 * 小米（HyperOS）專用的讀法
 * 每個廠商都有 4 個函式：
 *   xxx_line()          讀檔時，每一行都會丟進來看一次
 *   xxx_finish()        整個檔案讀完後，整理資料
 *   xxx_history()       讀 zip 解壓縮後的其他檔案
 *   xxx_history_page()  歷史紀錄頁，印出這個廠商才有的項目
 */

int g_xiaomi = 0;

/* 小米螢幕面板代碼 */
int panel_code[] = {36, 37, 38, 41, 42, 43};
const char *panel_name[] = {"天馬", "深超光電", "三星", "維信諾", "華星光電", "JDI"};
int panel_n = 6;

int xm_panel_code = -1;
char xm_fp_vendor[32] = "", xm_fp_module[32] = "";
int xm_poweroff_done = 0;

/* 開機失敗、差點卡死 */
struct Hist bootfail[MAX_LIST];
int g_nbootfail = 0;
char ta_app[MAX_LIST][96];
int ta_cnt[MAX_LIST];
int g_nta = 0, g_ta_total = 0;
char g_ta_first[16] = "", g_ta_last[16] = "";

/* ---------------- 讀檔時每一行 ---------------- */

void xiaomi_line(const char *section, const char *service)
{
    char *p;
    char tmp[64];
    int a;

    /* 系統參數 */
    if (line[0] == '[' && strcmp(section, "SYSTEM PROPERTIES") == 0) {
        if (get_prop(line, "ro.mi.os.version.incremental", g_os_ver, sizeof(g_os_ver))) strcpy(g_os_name, "HyperOS");
        if (get_prop(line, "ro.vendor.display.type", tmp, sizeof(tmp))) g_oled = 1;
        get_prop(line, "persist.vendor.sys.fp.vendor", xm_fp_vendor, sizeof(xm_fp_vendor));
        get_prop(line, "persist.vendor.sys.fp.module", xm_fp_module, sizeof(xm_fp_module));
        return;
    }

    /* 螢幕面板：Command line 裡的 panel-xxx-42 */
    if (starts_with(line, "Command line: ") && (p = strstr(line, "panel-")) != NULL) {
        char proj[16];
        copy_until(p + 6, '-', proj, sizeof(proj));
        sscanf(p + 6 + strlen(proj) + 1, "%d", &xm_panel_code);
    }

    /* 上次關機原因 */
    if (!xm_poweroff_done && strcmp(section, "POWEROFF_REASON") == 0 && line[0] != '\n' && line[0] != '\r') {
        copy_until(line, '\n', g_poweroff, sizeof(g_poweroff));
        trim(g_poweroff);
        if (strcmp(g_poweroff, "Normal_shutdown") == 0) {
            strcpy(g_poweroff, "正常關機");
            g_poweroff_ok = 1;
        } else {
            g_poweroff_ok = 0;
        }
        xm_poweroff_done = 1;
    }

    /* 記憶體和儲存晶片 */
    if (strcmp(section, "UFS/MMC mv") == 0) {
        if (starts_with(line, "D: ")) sscanf(line, "D: %x %d", &g_dram_code, &g_dram_gb);
        if (starts_with(line, "U: ")) sscanf(line, "U: %x %d %31s", &g_ufs_code, &g_ufs_gb, g_ufs_model);
    }
    if (strcmp(section, "PL_LK LOG") == 0) {
        if (g_dram_type[0] == '\0' && (p = strstr(line, "TYPE_LPDDR")) != NULL) {
            copy_until(p + 5, '!', g_dram_type, sizeof(g_dram_type));
        }
        if (g_dram_freq == 0 && (p = strstr(line, "Highest Freq ")) != NULL) sscanf(p + 13, "%d", &g_dram_freq);
        if (g_dram_ch == 0 && (p = strstr(line, "EMI ch_cnt: ")) != NULL) sscanf(p + 12, "%d", &g_dram_ch);
        if (g_ufs_gear[0] == '\0' && (p = strstr(line, "HS-G")) != NULL) {
            g_ufs_gear[0] = 'H'; g_ufs_gear[1] = 'S'; g_ufs_gear[2] = '-'; g_ufs_gear[3] = 'G';
            g_ufs_gear[4] = p[4]; g_ufs_gear[5] = '\0';
        }
    }
    if (strcmp(section, "UFS health desc") == 0) {
        if ((p = strstr(line, "bDeviceLifeTimeEstA = ")) != NULL) sscanf(p + 22, "%x", &g_life);
        if ((p = strstr(line, "bPreEOLInfo = ")) != NULL) sscanf(p + 14, "%x", &g_preeol);
    }
    if (strcmp(section, "UFS show hba") == 0) {
        if ((p = strstr(line, "hba->ufs_version = ")) != NULL) sscanf(p + 19, "%x", &g_ufs_ver);
        if (starts_with(line, "ufs_event_") && strstr(line, "error_cnt") != NULL && (p = strchr(line, '=')) != NULL) {
            if (sscanf(p + 1, "%x", &a) == 1) g_ufs_err += a;
        }
    }

    /* 電池 */
    if (g_soh < 0 && (p = strstr(line, "Soh:")) != NULL) sscanf(p + 4, "%d", &g_soh);
    if (g_cycle < 0 && (p = strstr(line, "cycle_count=")) != NULL) sscanf(p + 12, "%d", &g_cycle);
    if (g_cv < 0 && strstr(line, "charger_routine_thread") != NULL && (p = strstr(line, " cv:")) != NULL) sscanf(p + 4, "%ld", &g_cv);
    if ((p = strstr(line, "shutdown voltage=")) != NULL) {
        p += 17;
        while (sscanf(p, "%d", &a) == 1) {
            if (g_sd_min < 0 || a < g_sd_min) g_sd_min = a;
            if (a > g_sd_max) g_sd_max = a;
            while (isdigit((unsigned char)*p)) p++;
            if (*p != ',') break;
            p++;
        }
    }

    /* 更新率設定 */
    if (strcmp(service, "settings") == 0 && strstr(line, "name:miui_refresh_rate ") && (p = strstr(line, "value:")) != NULL) {
        sprintf(g_hz_setting, "%d Hz", atoi(p + 6));
    }

    /* 背景凍結（小米叫 Greezer） */
    if (strcmp(service, "greezer") == 0 && starts_with(line, "20") && g_ngz < 40000) {
        char ts[40], type[8];
        int uid;
        if (sscanf(line, "%39s - %7s uid = %d", ts, type, &uid) == 3) {
            if (strcmp(type, "FZ") == 0 || strcmp(type, "THAW") == 0 || strcmp(type, "Died") == 0) {
                gz[g_ngz].uid = uid;
                gz[g_ngz].type = type[0];
                gz[g_ngz].packet = (strstr(line, "reason : packet") != NULL);
                gz[g_ngz].t = iso_to_sec(ts);
                g_ngz++;
            }
        }
    }
}

/* ---------------- 讀完之後整理 ---------------- */

void xiaomi_finish(void)
{
    const char *n;
    g_xiaomi = (strcmp(g_maker, "Xiaomi") == 0);

    /* 螢幕面板 */
    if (xm_panel_code >= 0) {
        n = lookup(xm_panel_code, panel_code, panel_name, panel_n);
        if (n != NULL) sprintf(g_panel_text, "%s (面板編號 %d)", n, xm_panel_code);
        else           sprintf(g_panel_text, "未知 (面板編號 %d)", xm_panel_code);
    }

    /* 指紋 */
    if (xm_fp_vendor[0]) {
        if (starts_with(xm_fp_vendor, "goodix_us")) strcpy(g_fp_text, "匯頂 Goodix 超聲波");
        else if (starts_with(xm_fp_vendor, "goodix")) strcpy(g_fp_text, "匯頂 Goodix 光學");
        else strcpy(g_fp_text, xm_fp_vendor);
        if (xm_fp_module[0]) {
            strcat(g_fp_text, " / 模組 ");
            strcat(g_fp_text, strcmp(xm_fp_module, "QTech") == 0 ? "丘鈦 QTech" : xm_fp_module);
        }
    }
}

/* ---------------- zip 裡的其他檔案 ---------------- */

/* 比較兩個 HyperOS 版本，例如 OS3.0.307.0 和 OS4.0.3.0 */
int version_cmp(const char *a, const char *b)
{
    int x[4] = {0, 0, 0, 0}, y[4] = {0, 0, 0, 0}, i;
    sscanf(a, "OS%d.%d.%d.%d", &x[0], &x[1], &x[2], &x[3]);
    sscanf(b, "OS%d.%d.%d.%d", &y[0], &y[1], &y[2], &y[3]);
    for (i = 0; i < 4; i++) {
        if (x[i] != y[i]) return x[i] - y[i];
    }
    return 0;
}

/* 系統更新紀錄：FS\mnt\rescue\update_engine_log\update_engine.20261116-192345 */
void xm_load_updates(void)
{
    char dir[1100], path[1300], best[32], last_target[32] = "", t[24], text[128];
    static char names[100][128];
    int n, k, i, ok;
    FILE *f;
    char *p;

    sprintf(dir, "%s" SEP "FS" SEP "mnt" SEP "rescue" SEP "update_engine_log", g_base);
    n = list_dir(dir, "update_engine.", names, 100);
    for (k = 0; k < n; k++) {
        sprintf(path, "%s" SEP "%s", dir, names[k]);
        f = fopen(path, "rb");
        if (f == NULL) continue;
        best[0] = '\0';
        ok = 0;
        while (fgets(line, LINE_SIZE, f) != NULL) {
            if (strstr(line, "Update successfully applied") != NULL) ok = 1;
            p = line;
            while ((p = strstr(p, "OS")) != NULL) {
                if (isdigit((unsigned char)p[2]) && p[3] == '.') {
                    char v[32];
                    int n1, n2, n3, n4;
                    char tail[4];
                    i = 0;
                    while ((isalnum((unsigned char)p[i]) || p[i] == '.') && i < 31) { v[i] = p[i]; i++; }
                    v[i] = '\0';
                    /* 正確的版本長這樣：OS4.0.3.0.XONCNXM（4 組數字 + 大寫字母） */
                    if (sscanf(v, "OS%d.%d.%d.%d.%1[A-Z]", &n1, &n2, &n3, &n4, tail) == 5 && n3 < 1000) {
                        if (best[0] == '\0' || version_cmp(v, best) > 0) strcpy(best, v);
                    }
                }
                p += 2;
            }
        }
        fclose(f);
        /* 只列「有裝好」的，而且版本跟上一次不一樣 */
        if (ok && best[0] != '\0' && strcmp(best, last_target) != 0) {
            p = names[k] + 14;        /* 20261116-192345 */
            sprintf(t, "%.4s-%.2s-%.2s %.2s:%.2s", p, p + 4, p + 6, p + 9, p + 11);
            sprintf(text, "更新到 %s", best);
            add_hist(upd, &g_nupd, t, text);
            strcpy(last_target, best);
        }
    }
}

/* 恢復原廠 / recovery 紀錄：FS\mnt\rescue\recovery\last_log、last_log.1 ... */
void xm_load_resets(void)
{
    const char *mon[12] = {"Jan","Feb","Mar","Apr","May","Jun","Jul","Aug","Sep","Oct","Nov","Dec"};
    char sub[100], m3[8];
    char start_time[24], caller_time[24];
    int k, i, wipe, day, year, found;
    FILE *f;
    char *p;

    for (k = 0; k < 10; k++) {
        if (k == 0) strcpy(sub, "mnt" SEP "rescue" SEP "recovery" SEP "last_log");
        else sprintf(sub, "mnt" SEP "rescue" SEP "recovery" SEP "last_log.%d", k);
        f = open_fs(sub);
        if (f == NULL) continue;
        wipe = 0; found = 0;
        start_time[0] = '\0'; caller_time[0] = '\0';
        while (fgets(line, LINE_SIZE, f) != NULL) {
            if ((p = strstr(line, "Starting recovery")) != NULL && (p = strstr(p, ") on ")) != NULL) {
                /* 例如 on Mon Oct 26 10:58:03 2026 */
                if (sscanf(p + 5, "%*s %3s %d %*s %d", m3, &day, &year) == 3) {
                    for (i = 0; i < 12; i++) if (strcmp(m3, mon[i]) == 0) break;
                    sprintf(start_time, "%04d-%02d-%02d", year, i + 1, day);
                    found = 1;
                }
            }
            if (starts_with(line, "[") && strstr(line, "Command:") != NULL) {
                if (strstr(line, "--wipe_data") != NULL) wipe = 1;
                if ((p = strstr(line, "caller=")) != NULL && (p = strchr(p, ',')) != NULL) {
                    /* 2026-10-26T18:57:41Z */
                    sprintf(caller_time, "%.10s %.5s", p + 1, p + 12);
                }
            }
        }
        fclose(f);
        if (!found) continue;
        if (wipe && caller_time[0]) add_hist(rst, &g_nrst, caller_time, "恢復原廠（從設定執行）");
        else if (wipe) add_hist(rst, &g_nrst, caller_time[0] ? caller_time : start_time, "恢復原廠");
        else add_hist(rst, &g_nrst, caller_time[0] ? caller_time : start_time, "進入 recovery 模式，沒有清除資料");
    }
    sort_hist(rst, g_nrst);
}

/* 過去的當機紀錄：FS\mnt\rescue\mqsas\stabilityrecord\Local*Record.txt */
void xm_load_crash_records(void)
{
    const char *files[3] = {"LocalHangRecord.txt", "LocalKernelRebootRecord.txt", "LocalSubSystemRestartRecord.txt"};
    char sub[200], t[24] = "", ver[40] = "", proc[64], text[128];
    int k;
    FILE *f;
    char *p;

    for (k = 0; k < 3; k++) {
        sprintf(sub, "mnt" SEP "rescue" SEP "mqsas" SEP "stabilityrecord" SEP "%s", files[k]);
        f = open_fs(sub);
        if (f == NULL) continue;
        while (fgets(line, LINE_SIZE, f) != NULL) {
            p = strchr(line, ':');
            if (p == NULL) continue;
            if (starts_with(line, "record time")) { copy_until(p + 1, '\n', t, sizeof(t)); trim(t); t[16] = '\0'; }
            else if (starts_with(line, "os version")) { copy_until(p + 1, '\n', ver, sizeof(ver)); trim(ver); }
            else if (starts_with(line, "process")) {
                copy_until(p + 1, '\n', proc, sizeof(proc));
                trim(proc);
                if (k == 0) sprintf(text, "系統卡住 (%s)  %s", proc, ver);
                else if (k == 1) sprintf(text, "系統核心異常，自動重開機  %s", ver);
                else if (strstr(proc, "modem")) sprintf(text, "行動網路晶片重新啟動  %s", ver);
                else sprintf(text, "子系統重新啟動 (%s)  %s", proc, ver);
                add_hist(crashes, &g_ncrashes, t, text);
            }
        }
        fclose(f);
    }
    sort_hist(crashes, g_ncrashes);
}

/* 開機失敗紀錄：FS\mnt\rescue\mqsas\BootFailure\BootFailure_..._2026-10-26_18-57-44.zip */
void xm_load_bootfail(void)
{
    char dir[1100], t[24];
    static char names[50][128];
    int n, k, i, real;
    char *p;

    sprintf(dir, "%s" SEP "FS" SEP "mnt" SEP "rescue" SEP "mqsas" SEP "BootFailure", g_base);
    n = list_dir(dir, "BootFailure_", names, 50);
    for (k = 0; k < n; k++) {
        p = strstr(names[k], "_20");
        while (p != NULL && strstr(p + 1, "_20") != NULL) p = strstr(p + 1, "_20");   /* 找最後一個日期 */
        if (p == NULL) continue;
        sprintf(t, "%.10s %.2s:%.2s", p + 1, p + 12, p + 15);
        /* 同一個小時內有恢復原廠，就不是真的開機失敗 */
        real = 1;
        for (i = 0; i < g_nrst; i++) {
            if (strncmp(rst[i].time, t, 13) == 0) real = 0;
        }
        if (real) {
            add_hist(bootfail, &g_nbootfail, t, "開機失敗");
            g_real_bootfail++;
        } else {
            add_hist(bootfail, &g_nbootfail, t, "同時間有恢復原廠，應該不是真的開機失敗");
        }
    }
}

/* 差點卡死：FS\data\anr\temp_anr_... 每個檔案前幾行有 Cmd line: App 名稱 */
void xm_load_temp_anr(void)
{
    char dir[1100], path[1300], app[96];
    static char names[600][128];
    int n, k, i, lines;
    FILE *f;

    sprintf(dir, "%s" SEP "FS" SEP "data" SEP "anr", g_base);
    n = list_dir(dir, "temp_anr_", names, 600);
    for (k = 0; k < n; k++) {
        sprintf(path, "%s" SEP "%s", dir, names[k]);
        f = fopen(path, "rb");
        if (f == NULL) continue;
        app[0] = '\0';
        lines = 0;
        while (lines < 30 && fgets(line, LINE_SIZE, f) != NULL) {
            lines++;
            if (starts_with(line, "Cmd line: ")) {
                copy_until(line + 10, ':', app, sizeof(app));   /* com.tencent.mm:push 只留 com.tencent.mm */
                break;
            }
        }
        fclose(f);
        if (app[0] == '\0') continue;
        g_ta_total++;
        if (g_ta_first[0] == '\0') sprintf(g_ta_first, "%.5s", names[k] + 14);      /* temp_anr_2026-10-29 → 10-29 */
        sprintf(g_ta_last, "%.5s", names[k] + 14);
        for (i = 0; i < g_nta; i++) if (strcmp(ta_app[i], app) == 0) break;
        if (i == g_nta && g_nta < MAX_LIST) { strcpy(ta_app[i], app); ta_cnt[i] = 0; g_nta++; }
        if (i < g_nta) ta_cnt[i]++;
    }
}

void xiaomi_history(void)
{
    xm_load_updates();
    xm_load_resets();
    xm_load_crash_records();
    xm_load_bootfail();
    xm_load_temp_anr();
}

/* ---------------- 歷史紀錄頁：小米才有的項目 ---------------- */

void xiaomi_history_page(void)
{
    int i, j, tmp;
    char ts[96];
    if (!g_xiaomi) return;

    printf("\n ---- 開機失敗紀錄 ----\n");
    if (g_nbootfail == 0) printf(" 沒有紀錄\n");
    for (i = 0; i < g_nbootfail; i++) printf(" %s  %s\n", bootfail[i].time, bootfail[i].text);

    printf("\n ---- App 差點卡死 ----\n");
    if (g_ta_total == 0) {
        printf(" 沒有紀錄\n");
        return;
    }
    printf(" %s ～ %s 共 %d 份\n", g_ta_first, g_ta_last, g_ta_total);
    /* 照份數排序，多的在前 */
    for (i = 0; i < g_nta - 1; i++) {
        for (j = 0; j < g_nta - 1 - i; j++) {
            if (ta_cnt[j] < ta_cnt[j+1]) {
                tmp = ta_cnt[j]; ta_cnt[j] = ta_cnt[j+1]; ta_cnt[j+1] = tmp;
                strcpy(ts, ta_app[j]); strcpy(ta_app[j], ta_app[j+1]); strcpy(ta_app[j+1], ts);
            }
        }
    }
    for (i = 0; i < g_nta && i < 8; i++) printf("  %-32s %d 份\n", ta_app[i], ta_cnt[i]);
}

/* ##################################################################
 * 4. 三星
 * ################################################################## */

/*
 * 三星（One UI）專用的讀法
 * 4 個函式：samsung_line、samsung_finish、samsung_history、samsung_history_page
 */

int g_samsung = 0;

/* 三星型號 → 名稱（只列常見的 S 系列） */
const char *ss_code[] = {"SM-S901", "SM-S906", "SM-S908", "SM-S911", "SM-S916", "SM-S918",
                         "SM-S921", "SM-S926", "SM-S928", "SM-S931", "SM-S936", "SM-S938"};
const char *ss_name[] = {"Galaxy S22", "Galaxy S22+", "Galaxy S22 Ultra", "Galaxy S23", "Galaxy S23+", "Galaxy S23 Ultra",
                         "Galaxy S24", "Galaxy S24+", "Galaxy S24 Ultra", "Galaxy S25", "Galaxy S25+", "Galaxy S25 Ultra"};
int ss_n = 12;

char ss_pda[32] = "", ss_panel[48] = "";
long ss_batt_usage = -1;
int ss_rr_mode = -1;

/* 開關機紀錄 */
struct Hist nopower[MAX_LIST];
int g_nnopower = 0;
char ss_last_ver[32] = "", ss_hist_from[24] = "";
char ss_old_first[32] = "";
int ss_old_count = 0;

/* 三星韌體版本的年月：S9080ZHSBGZH3 → 倒數第 3 碼是年（Z=2026），倒數第 2 碼是月（H=8 月） */
void fw_month(const char *ver, char *out)
{
    int n = strlen(ver);
    int y, m;
    out[0] = '\0';
    if (n < 4) return;
    y = 2001 + (ver[n-3] - 'A');
    m = ver[n-2] - 'A' + 1;
    if (y < 2015 || y > 2040 || m < 1 || m > 12) return;
    sprintf(out, "%d-%02d", y, m);
}

/* ---------------- 讀檔時每一行 ---------------- */

/* 開關機紀錄的一行：2026-09-14 00:35:31+0800 |  REBOOT  |       | REASON: recovery [-] */
void ss_power_line(void)
{
    char when[24], kind[16] = "", rest[128] = "", text[128];
    char *f1, *f2, *f3, *p;

    sprintf(when, "%.16s", line);
    if (ss_hist_from[0] == '\0') strcpy(ss_hist_from, when);
    g_has_hist = 1;
    f1 = strchr(line, '|');
    f2 = f1 ? strchr(f1 + 1, '|') : NULL;
    f3 = f2 ? strchr(f2 + 1, '|') : NULL;
    if (f1 != NULL) sscanf(f1 + 1, "%15s", kind);
    if (f3 != NULL) { copy_until(f3 + 1, '\n', rest, sizeof(rest)); trim(rest); }

    if (strcmp(kind, "ON") == 0) {
        /* 開機時會寫韌體版本，版本變了就是有更新 */
        char ver[32] = "", ym[16];
        sscanf(rest, "%31s", ver);
        if (ver[0] == '\0' || ver[0] == '/') return;
        if (ss_last_ver[0] != '\0' && strcmp(ver, ss_last_ver) != 0) {
            fw_month(ver, ym);
            sprintf(text, "更新到 %s (%s 版)", ver, ym);
            add_hist(upd, &g_nupd, when, text);
        }
        strcpy(ss_last_ver, ver);
    } else if (strcmp(kind, "REBOOT") == 0 || strcmp(kind, "SHUTDOWN") == 0) {
        char reason[64] = "";
        int yy, mo, dd, hh, mi, ss;
        if ((p = strstr(rest, "REASON: ")) != NULL) copy_until(p + 8, '[', reason, sizeof(reason));
        trim(reason);
        if (sscanf(line, "%d-%d-%d %d:%d:%d", &yy, &mo, &dd, &hh, &mi, &ss) == 6 && g_nsd < 200) {
            sd_time[g_nsd++] = days_of(yy, mo, dd) * 86400.0 + hh * 3600.0 + mi * 60.0 + ss;
        }
        g_poweroff_ok = 1;
        if (strcmp(reason, "userrequested") == 0) {
            strcpy(g_poweroff, strcmp(kind, "REBOOT") == 0 ? "使用者重新開機" : "使用者關機");
        } else if (strcmp(reason, "no power") == 0) {
            strcpy(g_poweroff, "沒電自動關機");
            add_hist(nopower, &g_nnopower, when, "沒電自動關機");
        } else if (strcmp(reason, "recovery") == 0) {
            strcpy(g_poweroff, "進入 recovery（通常是系統更新）");
        } else if (strcmp(reason, "reset settings") == 0) {
            strcpy(g_poweroff, "重設所有設定");
            add_hist(rst, &g_nrst, when, "重設所有設定（資料保留）");
        } else if (strcmp(reason, "RamPlusChanged") == 0) {
            strcpy(g_poweroff, "更改 RAM Plus 後重新開機");
        } else {
            sprintf(g_poweroff, "異常重開機 (%.30s)", reason);
            g_poweroff_ok = 0;
            sprintf(text, "異常重開機 (%s)", reason);
            add_hist(crashes, &g_ncrashes, when, text);
        }
    }
}

void samsung_line(const char *section, const char *service)
{
    char *p;
    char tmp[64];
    int a;

    /* 系統參數 */
    if (line[0] == '[' && strcmp(section, "SYSTEM PROPERTIES") == 0) {
        get_prop(line, "ro.build.PDA", ss_pda, sizeof(ss_pda));
        get_prop(line, "ro.boot.warranty_bit", g_warranty, sizeof(g_warranty));
        if (get_prop(line, "ro.build.version.oneui", tmp, sizeof(tmp))) {
            a = atoi(tmp);      /* 80000 → 8.0 */
            strcpy(g_os_name, "One UI");
            sprintf(g_os_ver, "%d.%d", a / 10000, (a / 100) % 100);
        }
        return;
    }

    /* 螢幕面板：Command line 裡的 dsi_display0=ss_dsi_panel_S6E3HAE_AMB681AZ01_WQHD: */
    if (starts_with(line, "Command line: ") && (p = strstr(line, "dsi_display0=ss_dsi_panel_")) != NULL) {
        copy_until(p + 26, ':', ss_panel, sizeof(ss_panel));
    }

    /* 儲存晶片 */
    if (strcmp(section, "UFS PART NUMBER") == 0 && isalnum((unsigned char)line[0])) sscanf(line, "%31s", g_ufs_model);
    if (strcmp(section, "UFS VENDOR") == 0 && isalnum((unsigned char)line[0])) {
        sscanf(line, "%31s", g_ufs_maker);
        if (strcmp(g_ufs_maker, "SAMSUNG") == 0) strcpy(g_ufs_maker, "三星 Samsung");
    }
    if (strcmp(section, "UFS LT") == 0 && isdigit((unsigned char)line[0]) && g_life < 0) g_life = atoi(line);
    if (strcmp(section, "UFS ERR SUMMARY") == 0) {
        /* OPERR : 0, UICCMD : 0, ... 把數字全部加起來 */
        p = line;
        while ((p = strstr(p, ": ")) != NULL) {
            g_ufs_err += atol(p + 2);
            p += 2;
        }
    }

    /* 更新率設定 */
    if (strcmp(service, "settings") == 0 && strstr(line, "name:refresh_rate_mode ") && (p = strstr(line, "value:")) != NULL) {
        ss_rr_mode = atoi(p + 6);
    }

    /* 電池 */
    if (g_soh < 0 && (p = strstr(line, "/efs/FactoryApp/bsoh ,value:")) != NULL) sscanf(p + 28, "%d", &g_soh);
    if (g_asoc < 0 && (p = strstr(line, "mSavedBatteryAsoc: [")) != NULL) sscanf(p + 20, "%d", &g_asoc);
    if (ss_batt_usage < 0 && (p = strstr(line, "mSavedBatteryUsage: [")) != NULL) sscanf(p + 21, "%ld", &ss_batt_usage);
    if (g_first_use[0] == '\0' && (p = strstr(line, "battery FirstUseDate: [")) != NULL) copy_until(p + 23, ']', g_first_use, sizeof(g_first_use));

    /* 背景凍結（三星叫 Freecess）：FreecessController: FZ : com.xxx(10325) [pid] reason: ... */
    if (is_log_line(line) && g_ngz < 40000 && (p = strstr(line, "FreecessController: ")) != NULL) {
        char *q = p + 20;
        int is_fz = starts_with(q, "FZ : ");
        int is_ufz = starts_with(q, "UFZ : ");
        if ((is_fz || is_ufz) && (q = strchr(q, '(')) != NULL) {
            gz[g_ngz].uid = atoi(q + 1);
            gz[g_ngz].type = is_fz ? 'F' : 'T';
            gz[g_ngz].packet = (strstr(line, "reason: ") != NULL && (strstr(line, "acket") != NULL || strstr(line, "network") != NULL));
            gz[g_ngz].t = log_to_sec(line, g_cap_year);
            g_ngz++;
            strcpy(g_freeze_note, "(三星只在系統紀錄裡留下最近這段時間的凍結資料)");
        }
    }

    /* 開關機紀錄 */
    if (strcmp(section, "AP RESET INFO EXTEND") == 0 && isdigit((unsigned char)line[0]) && line[4] == '-') {
        ss_power_line();
    }
}

/* ---------------- 讀完之後整理 ---------------- */

void samsung_finish(void)
{
    int i;
    char ym[16];
    g_samsung = (strcmp(g_maker, "samsung") == 0);
    if (!g_samsung) return;

    /* 型號名稱 */
    if (g_model[0] == '\0') {
        for (i = 0; i < ss_n; i++) {
            if (starts_with(g_model_code, ss_code[i])) strcpy(g_model, ss_name[i]);
        }
    }

    /* 韌體版本 */
    if (ss_pda[0]) {
        fw_month(ss_pda, ym);
        sprintf(g_fw, "%s (%s 版)", ss_pda, ym);
    }

    /* 螢幕面板：S6E 開頭或有 _AM 的是三星顯示 */
    if (ss_panel[0]) {
        g_oled = 1;
        if (starts_with(ss_panel, "S6E") || strstr(ss_panel, "_AM") != NULL)
            sprintf(g_panel_text, "三星顯示 Samsung Display (%s)", ss_panel);
        else
            strcpy(g_panel_text, ss_panel);
    }

    /* 更新率設定 */
    if (ss_rr_mode == 0) strcpy(g_hz_setting, "標準 (60 Hz)");
    else if (ss_rr_mode > 0) strcpy(g_hz_setting, "自動調整");

    /* 充放電次數：累計用掉的電量 ÷ 100% */
    if (g_cycle < 0 && ss_batt_usage > 0) {
        g_cycle = (int)(ss_batt_usage / 100);
        sprintf(g_cycle_note, "(由累計用電量 %ld%% 換算)", ss_batt_usage);
    }
}

/* ---------------- zip 裡的其他檔案 ---------------- */

/* recovery 紀錄：FS\cache\recovery\last_history
   每一筆開頭是 + [xx | 時間 | 韌體版本]，時間常常是錯的，所以用韌體版本推算年月 */
void samsung_history(void)
{
    char ver[32] = "", last[32] = "", reason[160] = "", ym[16], when[24];
    int wipe = 0, first = 1, end;
    FILE *f;
    char *p;

    f = open_fs("cache" SEP "recovery" SEP "last_history");
    if (f == NULL) return;
    while (1) {
        end = (fgets(line, LINE_SIZE, f) == NULL);
        /* 遇到下一筆的開頭（或檔案結尾），就把上一筆存起來 */
        if ((end || starts_with(line, "+ [")) && !first) {
            if (strcmp(ver, last) != 0) {
                if (ss_old_first[0] == '\0') strcpy(ss_old_first, ver);
                ss_old_count++;
                strcpy(last, ver);
            }
            if (wipe) {
                p = strrchr(reason, ',');
                /* 時間是 1 月 1 日 00:00 的代表手機當時時間還沒設定，不可信 */
                if (p != NULL && isdigit((unsigned char)p[1]) && strstr(p, "-01-01T00:00:00") == NULL) {
                    sprintf(when, "%.10s", p + 1);
                } else {
                    fw_month(ver, ym);
                    sprintf(when, "%s 左右", ym);
                }
                if (strstr(reason, "Family Link") != NULL) add_hist(rst, &g_nrst, when, "被 Family Link 遠端清除資料");
                else if (strstr(reason, "MasterClear") != NULL) add_hist(rst, &g_nrst, when, "恢復原廠（從設定執行）");
                else add_hist(rst, &g_nrst, when, "恢復原廠");
            }
        }
        if (end) break;
        if (starts_with(line, "+ [")) {
            first = 0;
            wipe = 0;
            reason[0] = '\0';
            ver[0] = '\0';
            p = strrchr(line, '|');
            if (p != NULL) sscanf(p + 1, "%31s", ver);
            p = strchr(ver, ']');
            if (p != NULL) *p = '\0';
        } else if (starts_with(line, "--wipe_data")) {
            wipe = 1;
        } else if (starts_with(line, "reboot_reason=")) {
            copy_until(line + 14, '\n', reason, sizeof(reason));
        }
    }
    fclose(f);
    sort_hist(rst, g_nrst);
}

/* ---------------- 歷史紀錄頁：三星才有的項目 ---------------- */

void samsung_history_page(void)
{
    int i;
    char ym[16];
    if (!g_samsung) return;

    printf("\n ---- 三星的紀錄 ----\n");
    if (ss_old_count > 0) {
        fw_month(ss_old_first, ym);
        printf(" 更早的更新: 從 %s (%s 版) 開始，recovery 一共記錄了 %d 個版本\n", ss_old_first, ym, ss_old_count);
    }
    if (ss_hist_from[0]) printf(" 開關機紀錄從 %s 開始\n", ss_hist_from);

    printf("\n ---- 沒電自動關機 ----\n");
    if (g_nnopower == 0) printf(" 沒有紀錄\n");
    else printf(" 共 %d 次\n", g_nnopower);
    for (i = 0; i < g_nnopower; i++) printf(" %s\n", nopower[i].time);
}

/* ##################################################################
 * 5. OPPO
 * ################################################################## */

/*
 * OPPO（ColorOS）專用的讀法
 * OnePlus、realme 也是同一家公司的系統，一起放在這裡
 * 4 個函式：oppo_line、oppo_finish、oppo_history、oppo_history_page
 */

int g_oppo = 0;

char op_os_ver[16] = "", op_display_id[64] = "", op_panel[64] = "";
int op_soh = -1;

/* ---------------- 讀檔時每一行 ---------------- */

void oppo_line(const char *section, const char *service)
{
    char *p, *q;

    /* 系統參數 */
    if (line[0] == '[' && strcmp(section, "SYSTEM PROPERTIES") == 0) {
        get_prop(line, "ro.vendor.oplus.market.name", g_model, sizeof(g_model));
        get_prop(line, "ro.build.version.oplusrom.display", op_os_ver, sizeof(op_os_ver));
        get_prop(line, "ro.build.display.id", op_display_id, sizeof(op_display_id));
        return;
    }

    /* 螢幕面板和觸控：Command line 裡的
       mediatek_drm.dsi_display0=panel_ac180_p_3_a0020_dsi_cmd:PanelID-0x20480700
       oplus_bsp_tp_custom.dsi_display0=synaptics-s3910 */
    if (starts_with(line, "Command line: ")) {
        if ((p = strstr(line, "dsi_display0=panel_")) != NULL) {
            copy_until(p + 19, ' ', op_panel, sizeof(op_panel));
            q = strstr(op_panel, "_dsi");          /* 後面的 _dsi_cmd:PanelID... 不要 */
            if (q != NULL) *q = '\0';
        }
        if ((p = strstr(line, "tp_custom.dsi_display0=")) != NULL && g_touch[0] == '\0') {
            copy_until(p + 23, ' ', g_touch, sizeof(g_touch));
        }
    }

    /* 電池健康度：OPPO 電池 App 寫在系統設定裡的值，和「設定 → 電池 → 電池健康度」一樣 */
    if (strcmp(service, "settings") == 0 && strstr(line, "name:battery_tip_health_baseline_soh ") && (p = strstr(line, "value:")) != NULL) {
        op_soh = atoi(p + 6);
    }

    /* 電池：ChargerInfo 裡的 "batteryFcc":5255 是目前真正充滿的容量（mAh），讀不到健康度時用來推算 */
    if ((p = strstr(line, "\"batteryFcc\":")) != NULL) and_real_fcc_mah = atol(p + 13);

    /* 背景凍結（OPPO 叫 Hans）：
       OplusHansManager : freeze uid: 10411 com.instagram.android pids: [15559] scene: LcdOn
       OplusHansManager : unfreeze uid: 10411 com.instagram.android pids: [15559] reason: Signal scene: LcdOn */
    if (is_log_line(line) && g_ngz < 40000 && (p = strstr(line, "OplusHansManager : ")) != NULL) {
        p += 19;
        if (starts_with(p, "freeze uid: ") || starts_with(p, "unfreeze uid: ")) {
            int is_fz = (p[0] == 'f');
            gz[g_ngz].uid = atoi(strchr(p, ':') + 2);
            gz[g_ngz].type = is_fz ? 'F' : 'T';
            gz[g_ngz].packet = (!is_fz && strstr(p, "acket") != NULL);
            gz[g_ngz].t = log_to_sec(line, g_cap_year);
            g_ngz++;
            strcpy(g_freeze_note, "(OPPO 只在系統紀錄裡留下很短時間的凍結資料，參考就好)");
        }
    }
}

/* ---------------- 讀完之後整理 ---------------- */

void oppo_finish(void)
{
    g_oppo = (strcmp(g_maker, "OPPO") == 0 || strcmp(g_maker, "OnePlus") == 0 || strcmp(g_maker, "realme") == 0);
    if (!g_oppo) return;

    /* 系統名稱：OPPO 叫 ColorOS，OnePlus 叫 OxygenOS，realme 叫 realme UI */
    if (op_os_ver[0]) {
        if (strcmp(g_maker, "OnePlus") == 0) strcpy(g_os_name, "OxygenOS");
        else if (strcmp(g_maker, "realme") == 0) strcpy(g_os_name, "realme UI");
        else strcpy(g_os_name, "ColorOS");
        strcpy(g_os_ver, op_os_ver);
    }
    if (op_display_id[0]) strcpy(g_fw, op_display_id);

    /* 電池健康度：有電池 App 的值就用它 */
    if (g_soh < 0 && op_soh > 0 && op_soh <= 100) {
        g_soh = op_soh;
        strcpy(g_cycle_note, "(健康度是 ColorOS 電池設定裡記錄的值)");
    }

    /* 凍結紀錄不到 10 分鐘的話，算出來的比例沒有意義，就不顯示 */
    if (g_ngz > 0 && gz[g_ngz - 1].t - gz[0].t < 600) g_ngz = 0;

    /* 螢幕面板：報告裡只有代號，沒有寫廠商 */
    if (op_panel[0]) sprintf(g_panel_text, "面板代號 %s (報告裡沒有寫廠商)", op_panel);
}

/* ---------------- zip 裡的其他檔案（目前沒有找到 OPPO 專用的） ---------------- */

void oppo_history(void)
{
}

/* ---------------- 歷史紀錄頁（目前沒有 OPPO 專用的項目） ---------------- */

void oppo_history_page(void)
{
}

/* ##################################################################
 * 6. vivo
 * ################################################################## */

/*
 * vivo（OriginOS / Funtouch OS）專用的讀法，iQOO 也是 vivo
 * 4 個函式：vivo_line、vivo_finish、vivo_history、vivo_history_page
 */

int g_vivo = 0;

char vv_os_display[32] = "", vv_os_name[32] = "", vv_os_ver[16] = "", vv_display_id[64] = "";
char vv_panel[80] = "";
int vv_soh_fg = -1, vv_soh_show = -1;

/* ---------------- 讀檔時每一行 ---------------- */

void vivo_line(const char *section, const char *service)
{
    char *p, *q;
    char tmp[32];

    /* 系統參數 */
    if (line[0] == '[' && strcmp(section, "SYSTEM PROPERTIES") == 0) {
        get_prop(line, "ro.vivo.market.name", g_model, sizeof(g_model));
        get_prop(line, "ro.vivo.os.build.display.id", vv_os_display, sizeof(vv_os_display));   /* OriginOS 6 */
        get_prop(line, "ro.vivo.os.name", vv_os_name, sizeof(vv_os_name));                     /* Funtouch */
        get_prop(line, "ro.vivo.os.version", vv_os_ver, sizeof(vv_os_ver));                     /* 16.0 */
        get_prop(line, "ro.vivo.build.version", vv_display_id, sizeof(vv_display_id));
        if (get_prop(line, "persist.vivo.phone.panel_type", tmp, sizeof(tmp)) && strstr(tmp, "moled") != NULL) g_oled = 1;
        return;
    }

    /* 螢幕面板：Command line 裡的 msm_drm.dsi_display0=qcom,mdss_dsi_pd2454_cpd2378_wqhd_plus_dsc_cmd_dvt5: */
    if (starts_with(line, "Command line: ") && (p = strstr(line, "dsi_display0=qcom,mdss_dsi_")) != NULL) {
        copy_until(p + 27, ':', vv_panel, sizeof(vv_panel));
        q = strstr(vv_panel, "_wqhd");          /* 解析度以後的不要 */
        if (q == NULL) q = strstr(vv_panel, "_fhd");
        if (q != NULL) *q = '\0';
    }

    /* 電池：核心紀錄裡的數字（之後確認哪一個和手機設定一樣） */
    if ((p = strstr(line, "VFG:fg_common_battery_info")) != NULL && (q = strstr(p, ",soh=")) != NULL) vv_soh_fg = atoi(q + 5);
    if ((p = strstr(line, "battery_health_learning_soh_show: ")) != NULL) vv_soh_show = atoi(p + 34);

    /* 電池電壓範圍：充電上限、關機電壓（mV） */
    if ((p = strstr(line, "vfloat_voltage_show: ")) != NULL && g_cv < 0) g_cv = atol(p + 21);
    if ((p = strstr(line, "shutdown_voltage: val ")) != NULL && g_sd_min < 0) {
        g_sd_min = atoi(p + 22);
        g_sd_max = g_sd_min;
    }

    /* 背景凍結：
       _V_FreezerManager: onFrozenPackageForAudioVideo: pkgName=com.android.chrome, uid=10261, isFrozen=true, ...
       _V_FreezerManager: thaw com.android.chrome(10261) pids:{...} by fast_freezer net im      ← 收到訊息被叫醒 */
    if (is_log_line(line) && g_ngz < 40000 && (p = strstr(line, "_V_FreezerManager: ")) != NULL) {
        p += 19;
        if (starts_with(p, "onFrozenPackageForAudioVideo: ") && strstr(p, "isFrozen=true") != NULL && (q = strstr(p, "uid=")) != NULL) {
            gz[g_ngz].uid = atoi(q + 4);
            gz[g_ngz].type = 'F';
            gz[g_ngz].packet = 0;
            gz[g_ngz].t = log_to_sec(line, g_cap_year);
            g_ngz++;
        } else if (starts_with(p, "thaw ") && (q = strchr(p, '(')) != NULL) {
            gz[g_ngz].uid = atoi(q + 1);
            gz[g_ngz].type = 'T';
            gz[g_ngz].packet = (strstr(p, " net im") != NULL);
            gz[g_ngz].t = log_to_sec(line, g_cap_year);
            g_ngz++;
        }
    }
}

/* ---------------- 讀完之後整理 ---------------- */

void vivo_finish(void)
{
    char *p;
    g_vivo = (strcmp(g_maker, "vivo") == 0);
    if (!g_vivo) return;

    /* 系統名稱：中國版是 OriginOS，海外版是 Funtouch OS */
    if (vv_os_display[0] && (p = strrchr(vv_os_display, ' ')) != NULL) {
        *p = '\0';
        strcpy(g_os_name, vv_os_display);      /* OriginOS */
        strcpy(g_os_ver, p + 1);               /* 6 */
    } else if (vv_os_name[0]) {
        sprintf(g_os_name, "%s OS", vv_os_name);
        strcpy(g_os_ver, vv_os_ver);
    }
    if (vv_display_id[0]) strcpy(g_fw, vv_display_id);

    /* 電池健康度：用 vivo 要顯示在「設定 → 電池 → 電池健康」的值 */
    if (g_soh < 0 && vv_soh_show > 0 && vv_soh_show <= 100) {
        g_soh = vv_soh_show;
        strcpy(g_cycle_note, "(健康度是 vivo 電池設定裡顯示的值)");
    }

    /* 螢幕面板：報告裡只有代號，沒有寫廠商 */
    if (vv_panel[0]) sprintf(g_panel_text, "面板代號 %s (報告裡沒有寫廠商)", vv_panel);

    /* 凍結紀錄不到 10 分鐘的話，算出來的比例沒有意義，就不顯示 */
    if (g_ngz > 0 && gz[g_ngz - 1].t - gz[0].t < 600) g_ngz = 0;
    if (g_ngz > 0) strcpy(g_freeze_note, "(vivo 只在系統紀錄裡留下最近這段時間的凍結資料)");
}

/* ---------------- zip 裡的其他檔案（目前沒有找到 vivo 專用的） ---------------- */

void vivo_history(void)
{
}

/* ---------------- 歷史紀錄頁（目前沒有 vivo 專用的項目） ---------------- */

void vivo_history_page(void)
{
}

/* ##################################################################
 * 6.5 更多資料（每家手機都有的部分）
 *     溫度、感測器、音訊與震動、藍牙、應用程式、目前電池狀態、
 *     儲存空間用量、目前 Wi-Fi、行動網路、系統細節、螢幕細節
 * ################################################################## */

/* 系統細節 */
char x_baseband[64] = "", x_build_date[48] = "", x_kernel[48] = "", x_locale[24] = "", x_tz[40] = "";
char x_crypto[16] = "", x_slot[8] = "", x_region[16] = "", x_gms[24] = "";
char x_sim_op[96] = "", x_net_type[48] = "";
int x_first_api = -1;

/* 目前電池狀態（dumpsys battery） */
int xb_level = -1, xb_volt = -1, xb_temp = -9999, xb_status = -1, xb_health = -1;
long xb_counter = -1;
char xb_tech[16] = "";
int xb_ac = 0, xb_usb = 0, xb_wl = 0;

/* 耗電排行（batterystats） */
int xp_state = 0;                 /* 0 還沒遇到、1 正在讀、2 讀完 */
long xp_capacity = -1;
char xp_onbatt[48] = "", xp_screen[48] = "";
struct PowerUse { char uid[16]; double mah; };
struct PowerUse xp[400];
int xp_n = 0;

/* 套件名稱和 uid 的對照 */
char pk_name[1500][96];
int pk_uid[1500];
int pk_n = 0, pk_wait = -1;

/* 儲存空間用量（diskstats） */
double xd_app = -1, xd_data = -1, xd_cache = -1, xd_photo = -1, xd_video = -1, xd_audio = -1, xd_other = -1;
long xd_free_kb = -1, xd_write = -1;
char dn_name[1500][96];
double dn_size[1500];
int dn_n = 0;

/* 溫度（thermalservice） */
double xt[16];
int xt_status = -1, xt_inhal = 0, xt_done = 0;

/* 感測器 */
struct SensorInfo { int type; char name[64]; char vendor[24]; };
struct SensorInfo xs[16];
int xs_n = 0, xs_state = 0, xs_total = 0;

/* 螢幕細節 */
int xdp_density = 0;
double xdp_dpi = 0;
char xdp_hdr[80] = "";
double xdp_rates[16];
int xdp_nrates = 0;

/* 目前的 Wi-Fi 連線 */
char xw_std[16] = "";
int xw_rssi = 999, xw_link = -1, xw_freq = -1;

/* 記憶體目前的狀態 */
long xm_avail = -1, xm_swap_total = -1, xm_swap_free = -1;

/* 藍牙 */
char xbt_state[16] = "", xbt_name[64] = "";
int xbt_bonded = -1, xbt_in_bonded = 0, xbt_ndev = 0;
char xbt_dev[20][64];

/* 音訊與震動 */
char xa_chip[24] = "", xa_haptic[48] = "";
char xa_addr[8][16];
int xa_naddr = 0, xa_aw = 0, xa_stereo = 0;

/* 指紋（三星、vivo 的晶片型號） */
char x_fp[32] = "";

/* 套件的版本、安裝 / 更新時間、是不是系統內建 */
char pk_ver[1500][24];
char pk_first[1500][20], pk_last[1500][20];
char pk_sys[1500];

/* App 使用時間（usagestats 的每週統計） */
struct AppUse { char pkg[96]; long sec; };
struct AppUse us[800];
int us_n = 0, us_state = 0;
char us_range[64] = "";

/* 各 App 的網路流量（開機以來） */
struct NetUse { int uid; double rx, tx; };
struct NetUse nt[800];
int nt_n = 0, nt_state = 0;

/* 不讓手機休眠的元件、叫醒手機的原因 */
struct WakeItem { char who[16]; char name[96]; char dur[32]; long times; };
struct WakeItem wl[10], wr[8];
int wl_n = 0, wl_state = 0, wr_n = 0, wr_state = 0;

/* 權限使用紀錄（appops） */
const char *perm_op[] = {"FINE_LOCATION", "COARSE_LOCATION", "CAMERA", "RECORD_AUDIO", "READ_CLIPBOARD", "READ_CONTACTS", "READ_CALL_LOG", "READ_SMS"};
int perm_cat[] = {0, 0, 1, 2, 3, 4, 5, 6};
const char *perm_cn[] = {"位置", "相機", "麥克風", "剪貼簿", "聯絡人", "通話紀錄", "簡訊"};
int perm_n = 8, perm_cat_n = 7;
struct PermUse { char pkg[64]; int cat; char t[20]; };
struct PermUse pu[1200];
int pu_n = 0, ap_op = -1;
char ap_pkg[64] = "";

/* 充電紀錄（Battery History） */
struct ChargeSession { char start[16]; char end[16]; int from, to; char plug[12]; long sec; };
struct ChargeSession cs[15];
int cs_n = 0, bh_state = 0, bh_charging = 0, bh_prev = -1;
double bh_prev_t = 0, bh_first_t = 0, bh_last_t = 0, bh_dis_sec = 0, bh_start_t = 0;
long bh_drop = 0;
char bh_first[16] = "", bh_lastst[16] = "", bh_plug[12] = "";
struct DrainHour { char key[12]; int drop; };
struct DrainHour dh[400];
int dh_n = 0;
struct ChargeSession cs_cur;

/* 鬧鐘叫醒手機的次數（alarm） */
struct AlarmUse { char pkg[96]; long wakes; };
struct AlarmUse al[300];
int al_n = 0, al_state = 0;

/* 定位（GNSS） */
long gn_reports = -1, gn_ttff_n = -1, gn_l5 = -1;
double gn_fail = -1, gn_ttff = -1, gn_acc = -1, gn_cn0 = -1;
char gn_const[64] = "";

/* ---------- 安全檢查 ---------- */
char sc_lock[16] = "";                        /* PIN、PASSWORD、PATTERN、NONE */
int sc_face_n = -1, sc_face_ok = 0, sc_face_ng = 0;
int sc_fp_n = -1, sc_fp_ok = 0, sc_fp_ng = 0;
char sc_a11y[512] = "";                       /* 開了無障礙服務的（設定裡的原始值） */
char sc_admin[8][96];
int sc_admin_n = 0, sc_admin_state = 0;
char sc_install[40][64];                      /* 可以安裝未知來源 App 的 */
int sc_install_n = 0, sc_install_op = 0;
char sc_dns_mode[24] = "", sc_dns_host[64] = "", sc_vpn[96] = "";
int sc_unknown_src = -1;

/* ---------- 設定狀態 ---------- */
int st_bright = -1, st_bright_auto = -1, st_night = -1, st_low_power = -1, st_zen = -1;
long st_timeout = -1;
double st_font = -1, st_anim = -1, st_trans = -1, st_win = -1;
char st_white[60][64];                        /* 不受電池最佳化限制的 App */
int st_white_n = 0, st_white_state = 0;

/* ---------- 效能 ---------- */
struct ProcUse { char name[64]; double val; };
struct ProcUse pm[10], pc[10];                /* 佔記憶體、吃處理器 */
int pm_n = 0, pm_state = 0, pc_n = 0, pc_state = 0;
char pm_title[24] = "";
long mi_total = -1, mi_free = -1, mi_used = -1, mi_lost = -1, mi_zram = -1, mi_swap_used = -1;
char mi_status[16] = "", pc_load[32] = "";

/* ---------- 通知與相機 ---------- */
struct CountItem { char name[96]; int n; };
struct CountItem nf[200], cam[60];
int nf_n = 0, cam_n = 0;

/* ---------- 進階數據 ---------- */
struct WakeItem kw[10];
int kw_n = 0, kw_state = 0;
struct TempItem { char name[24]; double v; };
struct TempItem tz[40];
int tz_n = 0, tz_state = 0;

/* ---------- 原始資料：每個區段在檔案裡的位置 ---------- */
struct RawSec { char name[80]; long start; char kind; };
struct RawSec rs[3000];
int rs_n = 0;
long rs_file_size = 0;
char rs_path[1000] = "";

/* 原始資料要遮掉的字（Wi-Fi 名稱、裝置名稱，常常含有人名或地址） */
char mk[150][64];
int mk_n = 0;

/* 之後才定義的函式（在各分頁裡呼叫） */
void ex_system_more(void);
void ex_cpu_more(void);
void ex_memory_more(void);
void ex_storage_more(void);
void ex_battery_now(void);
void ex_network_more(void);
void ex_amp_line(void);
void note(const char *s);
void add_mask(const char *s);

/* ---------------- 小工具 ---------------- */

/* 位元組換成「12.3 GB」 */
void bytes_text(double b, char *out)
{
    if (b >= 1024.0 * 1024 * 1024) sprintf(out, "%.1f GB", b / (1024.0 * 1024 * 1024));
    else if (b >= 1024.0 * 1024)   sprintf(out, "%.0f MB", b / (1024.0 * 1024));
    else                           sprintf(out, "%.0f KB", b / 1024.0);
}

/* 「14h 1m 33s 987ms」或「3d 17h 26m 9s」換成「14 小時 1 分」 */
void dur_text(const char *s, char *out)
{
    long d = 0, h = 0, m = 0, sec = 0, v;
    const char *p = s;
    while (*p) {
        if (!isdigit((unsigned char)*p)) { p++; continue; }
        v = atol(p);
        while (isdigit((unsigned char)*p)) p++;
        if (*p == 'd') d = v;
        else if (*p == 'h') h = v;
        else if (*p == 'm' && p[1] != 's') m = v;
        else if (*p == 's') sec = v;
        else if (*p == 'm' && p[1] == 's') break;
        if (*p) p++;
    }
    if (d > 0) sprintf(out, "%ld 天 %ld 小時 %ld 分", d, h, m);
    else if (h > 0) sprintf(out, "%ld 小時 %ld 分", h, m);
    else if (m > 0) sprintf(out, "%ld 分 %ld 秒", m, sec);
    else sprintf(out, "%ld 秒", sec);
}

/* 加一個鏡頭感光元件（轉成大寫、不重複） */
void add_cam(const char *name)
{
    char up[16];
    int i;
    for (i = 0; name[i] && i < 15; i++) up[i] = toupper((unsigned char)name[i]);
    up[i] = '\0';
    if (up[0] == '\0') return;
    for (i = 0; i < g_ncam; i++) if (strcmp(g_cams[i], up) == 0) return;
    if (g_ncam < 8) strcpy(g_cams[g_ncam++], up);
}

/* 常見 App 的中文名字 */
const char *app_pkg[] = {
    "com.google.android.gms", "com.android.vending", "com.google.android.youtube", "com.android.chrome",
    "jp.naver.line.android", "com.facebook.katana", "com.facebook.orca", "com.instagram.android",
    "com.tencent.mm", "com.tencent.mobileqq", "com.ss.android.ugc.aweme", "com.zhiliaoapp.musically",
    "tv.danmaku.bili", "com.whatsapp", "org.telegram.messenger", "com.google.android.apps.maps",
    "com.google.android.apps.photos", "com.spotify.music", "com.netflix.mediaclient", "com.shopee.tw",
    "com.twitter.android", "com.discord", "com.google.android.googlequicksearchbox", "com.miui.home",
    "com.sec.android.app.launcher", "com.android.systemui", "com.taobao.taobao", "com.eg.android.AlipayGphone",
    "com.sina.weibo", "com.xingin.xhs", "com.zhihu.android", "com.baidu.BaiduMap",
    "com.autonavi.minimap", "com.google.android.apps.youtube.music", "com.kuaishou.nebula", "com.tencent.qqlive",
    "com.netease.cloudmusic", "com.google.android.inputmethod.latin", "com.android.camera", "com.android.settings",
    "android", "com.xiaomi.xmsf", "com.android.contacts", "com.android.mms", "com.android.deskclock",
    "com.miui.securitycenter", "com.android.bluetooth", "com.android.updater", "com.miui.voiceassist", "com.xiaomi.market",
    "com.android.phone", "com.google.android.apps.walletnfcrel", "com.google.android.apps.translate", "com.instagram.barcelona",
    "com.roblox.client", "com.garena.game.codm", "com.garena.game.kgtw", "flar2.devcheck",
    "com.anthropic.claude", "com.openai.chatgpt", "com.ss.android.ugc.trill", "com.tencent.android.qqdownloader",
    "com.larus.nova", "com.aliyun.tongyi", "com.luna.music", "com.bbk.appstore",
    "com.heytap.market", "com.sec.android.app.samsungapps", "com.oplus.camera", "com.android.launcher",
    "com.coloros.gallery3d", "com.miui.gallery", "com.vivo.gallery", "com.sec.android.gallery3d",
    "com.android.server.telecom", "com.vivo.pushservice"
};
const char *app_cn[] = {
    "Google Play 服務", "Play 商店", "YouTube", "Chrome",
    "LINE", "Facebook", "Messenger", "Instagram",
    "微信", "QQ", "抖音", "TikTok",
    "嗶哩嗶哩", "WhatsApp", "Telegram", "Google 地圖",
    "Google 相簿", "Spotify", "Netflix", "蝦皮購物",
    "X (Twitter)", "Discord", "Google", "小米桌面",
    "三星桌面", "系統介面", "淘寶", "支付寶",
    "微博", "小紅書", "知乎", "百度地圖",
    "高德地圖", "YouTube Music", "快手", "騰訊視頻",
    "網易雲音樂", "Gboard 鍵盤", "相機", "設定",
    "Android 系統", "小米推播服務", "聯絡人", "簡訊", "時鐘",
    "小米手機管家", "藍牙", "系統更新", "小愛同學", "小米應用商店",
    "電話服務", "Google 錢包", "Google 翻譯", "Threads",
    "Roblox", "決勝時刻 Mobile", "傳說對決", "DevCheck",
    "Claude", "ChatGPT", "TikTok", "應用寶",
    "豆包", "通義", "汽水音樂", "vivo 應用商店",
    "OPPO 軟體商店", "Galaxy Store", "OPPO 相機", "OPPO 桌面",
    "OPPO 相簿", "小米相簿", "vivo 相簿", "三星相簿",
    "通話服務", "vivo 推播服務"
};
int app_n = sizeof(app_cn) / sizeof(app_cn[0]);

/* 套件名稱 → 顯示用的名字 */
void app_label(const char *pkg, char *out)
{
    int i;
    for (i = 0; i < app_n; i++) {
        if (strcmp(pkg, app_pkg[i]) == 0) { sprintf(out, "%s (%s)", app_cn[i], pkg); return; }
    }
    strcpy(out, pkg);
}

/* 耗電排行的 uid（u0a357、1000）→ 名字 */
void uid_label(const char *uid, char *out)
{
    int n, i;
    char tmp[160];
    if (uid[0] == 'u' && uid[1] == '0' && uid[2] == 'a') {
        n = 10000 + atoi(uid + 3);
        for (i = 0; i < pk_n; i++) {
            if (pk_uid[i] == n) { app_label(pk_name[i], tmp); sprintf(out, "%.90s", tmp); return; }
        }
        sprintf(out, "App (uid %d)", n);
        return;
    }
    if (!isdigit((unsigned char)uid[0])) { strcpy(out, uid); return; }
    n = atoi(uid);
    if (n == 0) strcpy(out, "系統核心");
    else if (n == 1000) strcpy(out, "Android 系統");
    else if (n == 1001) strcpy(out, "電話服務");
    else if (n == 1002) strcpy(out, "藍牙");
    else if (n == 1010) strcpy(out, "Wi-Fi");
    else if (n == 1013) strcpy(out, "媒體服務");
    else if (n == 1021) strcpy(out, "定位 (GPS)");
    else if (n == 1027) strcpy(out, "NFC");
    else if (n == 1041) strcpy(out, "音訊服務");
    else if (n == 1046) strcpy(out, "影音編解碼");
    else if (n == 1047) strcpy(out, "相機服務");
    else sprintf(out, "系統服務 (uid %d)", n);
}

/* 感測器種類 → 中文（只列實體感測器） */
const char *sensor_cn(int type)
{
    switch (type) {
    case 1: return "加速度計";
    case 2: return "磁力計 (指南針)";
    case 4: return "陀螺儀";
    case 5: return "光線感測器";
    case 6: return "氣壓計";
    case 8: return "距離感測器";
    case 12: return "濕度感測器";
    case 13: return "溫度感測器";
    case 21: return "心率感測器";
    }
    return NULL;
}

/* 讀 JSON 字串陣列 ["a","b"] 到 dn_name */
void read_name_array(const char *p)
{
    const char *q;
    int len;
    dn_n = 0;
    while ((p = strchr(p, '"')) != NULL && dn_n < 1500) {
        q = strchr(p + 1, '"');
        if (q == NULL) break;
        len = q - (p + 1);
        if (len > 95) len = 95;
        strncpy(dn_name[dn_n], p + 1, len);
        dn_name[dn_n][len] = '\0';
        dn_size[dn_n] = 0;
        dn_n++;
        p = q + 1;
    }
}

/* 讀數字陣列 [1,2,3]，加到 dn_size */
void add_size_array(const char *p)
{
    int i = 0;
    p = strchr(p, '[');
    if (p == NULL) return;
    p++;
    while (*p && *p != ']' && i < dn_n) {
        dn_size[i] += atof(p);
        while (*p && *p != ',' && *p != ']') p++;
        if (*p == ',') p++;
        i++;
    }
}


/* 「8:43:31」或「55:43」換成秒 */
long hms_to_sec(const char *s)
{
    long v[4] = {0, 0, 0, 0};
    int n = 0;
    while (*s && n < 4) {
        v[n++] = atol(s);
        while (isdigit((unsigned char)*s)) s++;
        if (*s != ':') break;
        s++;
    }
    if (n == 2) return v[0] * 60 + v[1];
    if (n == 3) return v[0] * 3600 + v[1] * 60 + v[2];
    if (n == 4) return v[0] * 86400 + v[1] * 3600 + v[2] * 60 + v[3];
    return 0;
}

/* 秒換成「3 小時 5 分」 */
void sec_text2(long s, char *out)
{
    if (s >= 3600) sprintf(out, "%ld 小時 %ld 分", s / 3600, (s % 3600) / 60);
    else if (s >= 60) sprintf(out, "%ld 分", s / 60);
    else sprintf(out, "%ld 秒", s);
}

/* uid 數字 → 名字 */
void uid_num_label(int uid, char *out)
{
    char u[16];
    if (uid >= 10000 && uid < 20000) sprintf(u, "u0a%d", uid - 10000);
    else sprintf(u, "%d", uid);
    uid_label(u, out);
}

/* 「Wake lock u0a318 MicroMsg.Xxx: 1m 31s 921ms (266 times) ...」
   「Wakeup reason 345 rcs_irq: 7m 52s 277ms (344 times) realtime」 */
int parse_wake(const char *s, int has_who, struct WakeItem *w)
{
    const char *tm = strstr(s, " times)");
    const char *op, *c, *p = s;
    int len;
    if (tm == NULL) return 0;
    op = tm;
    while (op > s && *op != '(') op--;
    c = op;
    while (c > s && !(c[0] == ':' && c[1] == ' ')) c--;
    if (c <= s) return 0;
    w->who[0] = '\0';
    if (has_who) {
        len = 0;
        while (p[len] && p[len] != ' ' && len < 15) len++;
        strncpy(w->who, p, len);
        w->who[len] = '\0';
        p += len;
        while (*p == ' ') p++;
    }
    len = c - p;
    if (len > 95) len = 95;
    if (len < 0) len = 0;
    strncpy(w->name, p, len);
    w->name[len] = '\0';
    len = op - (c + 2);
    if (len > 31) len = 31;
    if (len < 0) len = 0;
    strncpy(w->dur, c + 2, len);
    w->dur[len] = '\0';
    trim(w->dur);
    w->times = atol(op + 1);
    return 1;
}

/* 叫醒手機的原因 → 白話 */
const char *wake_reason_cn(const char *r)
{
    char low[96];
    int i;
    for (i = 0; r[i] && i < 95; i++) low[i] = tolower((unsigned char)r[i]);
    low[i] = '\0';
    if (strstr(low, "modem") || strstr(low, "rcs") || strstr(low, "radio") || strstr(low, "ipa_") || strstr(low, "rmnet")) return "行動網路";
    if (strstr(low, "wlan") || strstr(low, "wifi")) return "Wi-Fi";
    if (strstr(low, "timerfd") || strstr(low, "alarm") || strstr(low, "rtc")) return "計時器 / 鬧鐘";
    if (strstr(low, "adsp") || strstr(low, "audio")) return "音訊 / 感測處理器";
    if (strstr(low, "bt") || strstr(low, "blue")) return "藍牙";
    if (strstr(low, "usb") || strstr(low, "charg") || strstr(low, "pmic")) return "充電 / USB";
    if (strstr(low, "touch") || strstr(low, "_ts") || strstr(low, "key") || strstr(low, "power")) return "按鍵 / 觸控";
    if (strstr(low, "sensor") || strstr(low, "ssc") || strstr(low, "scp")) return "感測器";
    return "其他";
}

/* 充電紀錄：結束一段充電 */
void bh_end_session(double t, const char *when, int lvl)
{
    int i;
    if (t - bh_start_t < 120) return;          /* 不到 2 分鐘的不算 */
    if (lvl <= cs_cur.from && t - bh_start_t < 600) return;   /* 10 分鐘內電量沒增加的（例如無線充電一下接上一下斷開）也不算 */
    cs_cur.sec = (long)(t - bh_start_t);
    cs_cur.to = lvl;
    strcpy(cs_cur.end, when);
    if (cs_n == 15) {
        for (i = 0; i < 14; i++) cs[i] = cs[i + 1];
        cs_n = 14;
    }
    cs[cs_n++] = cs_cur;
}

/* 充電紀錄的一行：  11-17 20:38:05.435 062 84090018 status=charging plug=ac ... */
void bh_line(void)
{
    char md[8], hms[16], when[16], key[12], *p;
    int lvl, i, chg;
    double t;
    if (!(isdigit((unsigned char)line[2]) && line[4] == '-' && line[7] == ' ')) return;
    if (sscanf(line + 2, "%7s %15s %d", md, hms, &lvl) != 3 || lvl < 0 || lvl > 100) return;
    if (!isdigit((unsigned char)line[21])) return;
    t = log_to_sec(line + 2, g_cap_year);
    sprintf(when, "%s %.5s", md, hms);
    if (bh_first[0] == '\0') { strcpy(bh_first, when); bh_first_t = t; }
    strcpy(bh_lastst, when);
    bh_last_t = t;

    chg = bh_charging;
    if (strstr(line, " status=charging") || strstr(line, " status=full") || strstr(line, " +plugged")) chg = 1;
    if (strstr(line, " status=discharging") || strstr(line, " status=not-charging") || strstr(line, " -plugged")) chg = 0;
    if ((p = strstr(line, " plug=")) != NULL) {
        copy_until(p + 6, ' ', bh_plug, sizeof(bh_plug));
        if (bh_charging && strcmp(bh_plug, "none") != 0) strcpy(cs_cur.plug, bh_plug);
    }

    /* 沒插電時，電量掉了多少、花了多久 */
    if (!bh_charging && bh_prev >= 0) {
        if (t - bh_prev_t > 0 && t - bh_prev_t < 6 * 3600) bh_dis_sec += t - bh_prev_t;
        if (lvl < bh_prev) {
            bh_drop += bh_prev - lvl;
            sprintf(key, "%s %.2s", md, hms);
            for (i = 0; i < dh_n; i++) if (strcmp(dh[i].key, key) == 0) break;
            if (i == dh_n && dh_n < 400) { strcpy(dh[dh_n].key, key); dh[dh_n].drop = 0; dh_n++; }
            if (i < dh_n) dh[i].drop += bh_prev - lvl;
        }
    }

    if (chg && !bh_charging) {             /* 開始充電 */
        strcpy(cs_cur.start, when);
        cs_cur.from = lvl;
        strcpy(cs_cur.plug, strcmp(bh_plug, "none") == 0 ? "" : bh_plug);
        bh_start_t = t;
    } else if (!chg && bh_charging) {      /* 停止充電 */
        bh_end_session(t, when, lvl);
    }
    bh_charging = chg;
    bh_prev = lvl;
    bh_prev_t = t;
}

/* 名字出現一次就加 1 */
void add_count(struct CountItem *arr, int *n, int max, const char *name)
{
    int i;
    for (i = 0; i < *n; i++) if (strcmp(arr[i].name, name) == 0) { arr[i].n++; return; }
    if (*n < max) { strncpy(arr[*n].name, name, 95); arr[*n].name[95] = '\0'; arr[*n].n = 1; (*n)++; }
}

/* 「632,420K」換成數字（KB） */
double kb_num(const char *s)
{
    double v = 0;
    while (*s == ' ') s++;
    while (isdigit((unsigned char)*s) || *s == ',') {
        if (*s != ',') v = v * 10 + (*s - '0');
        s++;
    }
    return v;
}

/* 生物辨識：{"service":"FaceProvider/default","prints":[{"id":0,"count":1,"accept":22,"reject":3 */
void read_bio(const char *p, int *cnt, int *ok, int *ng)
{
    const char *q;
    if ((q = strstr(p, "\"count\":")) != NULL) *cnt = atoi(q + 8);
    if ((q = strstr(p, "\"accept\":")) != NULL) *ok = atoi(q + 9);
    if ((q = strstr(p, "\"reject\":")) != NULL) *ng = atoi(q + 9);
}

/* 系統設定的一筆：_id:12 name:font_scale pkg:android value:1.0 default:... */
void read_setting(const char *s)
{
    char name[64], val[512];
    const char *p = strstr(s, " name:"), *q = strstr(s, " value:");
    if (p == NULL || q == NULL) return;
    copy_until(p + 6, ' ', name, sizeof(name));
    copy_until(q + 7, ' ', val, sizeof(val));
    if (strcmp(val, "null") == 0 || strcmp(val, "{null}") == 0) val[0] = '\0';
    if (strcmp(name, "enabled_accessibility_services") == 0 && sc_a11y[0] == '\0') strcpy(sc_a11y, val[0] ? val : "-");
    else if (strcmp(name, "private_dns_mode") == 0 && sc_dns_mode[0] == '\0') strncpy(sc_dns_mode, val, 23);
    else if (strcmp(name, "private_dns_specifier") == 0 && sc_dns_host[0] == '\0') strncpy(sc_dns_host, val, 63);
    else if (strcmp(name, "always_on_vpn_app") == 0 && sc_vpn[0] == '\0') strncpy(sc_vpn, val, 95);
    else if (strcmp(name, "install_non_market_apps") == 0 && sc_unknown_src < 0) sc_unknown_src = atoi(val);
    else if (strcmp(name, "screen_brightness") == 0 && st_bright < 0) st_bright = atoi(val);
    else if (strcmp(name, "screen_brightness_mode") == 0 && st_bright_auto < 0) st_bright_auto = atoi(val);
    else if (strcmp(name, "screen_off_timeout") == 0 && st_timeout < 0) st_timeout = atol(val);
    else if (strcmp(name, "font_scale") == 0 && st_font < 0) st_font = atof(val);
    else if (strcmp(name, "animator_duration_scale") == 0 && st_anim < 0) st_anim = atof(val);
    else if (strcmp(name, "transition_animation_scale") == 0 && st_trans < 0) st_trans = atof(val);
    else if (strcmp(name, "window_animation_scale") == 0 && st_win < 0) st_win = atof(val);
    else if (strcmp(name, "ui_night_mode") == 0 && st_night < 0) st_night = atoi(val);
    else if (strcmp(name, "low_power") == 0 && st_low_power < 0) st_low_power = atoi(val);
    else if (strcmp(name, "zen_mode") == 0 && st_zen < 0) st_zen = atoi(val);
    else if (strcmp(name, "device_name") == 0 || strcmp(name, "bluetooth_name") == 0) add_mask(val);
}

/* 加一個要遮掉的字 */
void add_mask(const char *s)
{
    int i;
    if (strlen(s) < 3 || s[0] == '<') return;          /* 太短的、<unknown ssid> 不用 */
    for (i = 0; i < mk_n; i++) if (strcmp(mk[i], s) == 0) return;
    if (mk_n < 150) { strncpy(mk[mk_n], s, 63); mk[mk_n][63] = '\0'; mk_n++; }
}

/* 原始資料：記下一個區段（在 read_file 裡讀到區段標題時呼叫） */
void raw_add(const char *title, char kind, long start)
{
    char *p;
    if (rs_n >= 3000) return;
    copy_until(title, '\n', rs[rs_n].name, sizeof(rs[rs_n].name));
    trim(rs[rs_n].name);
    p = strstr(rs[rs_n].name, " (");          /* 後面的指令不用顯示 */
    if (p != NULL) *p = '\0';
    p = strstr(rs[rs_n].name, " ------");
    if (p != NULL) *p = '\0';
    p = rs[rs_n].name + strlen(rs[rs_n].name);
    if (p > rs[rs_n].name && p[-1] == ':') p[-1] = '\0';
    rs[rs_n].start = start;
    rs[rs_n].kind = kind;
    rs_n++;
}

/* ---------------- 讀檔時每一行 ---------------- */

void ex_line(const char *section, const char *service)
{
    char *p, *q;
    char tmp[128];
    int a, i;

    /* 系統參數 */
    if (line[0] == '[' && strcmp(section, "SYSTEM PROPERTIES") == 0) {
        if (get_prop(line, "gsm.version.baseband", x_baseband, sizeof(x_baseband)) && (p = strchr(x_baseband, ',')) != NULL) *p = '\0';
        get_prop(line, "ro.build.date", x_build_date, sizeof(x_build_date));
        get_prop(line, "persist.sys.locale", x_locale, sizeof(x_locale));
        get_prop(line, "persist.sys.timezone", x_tz, sizeof(x_tz));
        get_prop(line, "ro.crypto.state", x_crypto, sizeof(x_crypto));
        get_prop(line, "ro.boot.slot_suffix", x_slot, sizeof(x_slot));
        get_prop(line, "ro.boot.hwc", x_region, sizeof(x_region));
        get_prop(line, "ro.com.google.gmsversion", x_gms, sizeof(x_gms));
        get_prop(line, "gsm.sim.operator.alpha", x_sim_op, sizeof(x_sim_op));
        get_prop(line, "gsm.network.type", x_net_type, sizeof(x_net_type));
        if (get_prop(line, "ro.product.first_api_level", tmp, sizeof(tmp))) x_first_api = atoi(tmp);
        if (get_prop(line, "ro.vendor.audio.spk.stereo", tmp, sizeof(tmp)) && strcmp(tmp, "true") == 0) xa_stereo = 1;
        /* vivo 的鏡頭：[persist.vendor.camera.sensor1]: [s5kjn1] */
        if (starts_with(line, "[persist.vendor.camera.sensor") && isdigit((unsigned char)line[29]) && line[30] == ']') {
            copy_until(line + 34, ']', tmp, sizeof(tmp));
            q = strchr(tmp, '_');
            if (q != NULL) *q = '\0';           /* imx06c_wide → imx06c */
            if (strlen(tmp) >= 5 && (starts_with(tmp, "imx") || starts_with(tmp, "s5k") || starts_with(tmp, "ov") ||
                                     starts_with(tmp, "lyt") || starts_with(tmp, "gc") || starts_with(tmp, "sc") || starts_with(tmp, "hi")))
                add_cam(tmp);
        }
        return;
    }


    /* App 使用時間（每週統計） */
    if (strcmp(service, "usagestats") == 0 && us_state < 2) {
        if (strstr(line, "In-memory weekly stats") != NULL) us_state = 1;
        else if (us_state == 1 && strstr(line, "In-memory monthly stats") != NULL) us_state = 2;
        else if (us_state == 1) {
            if (us_range[0] == '\0' && (p = strstr(line, "timeRange=\"")) != NULL) {
                copy_until(p + 11, '"', us_range, sizeof(us_range));
                trim(us_range);
            }
            if ((p = strstr(line, "package=")) != NULL && (q = strstr(line, "totalTimeUsed=\"")) != NULL && us_n < 800) {
                long sec = hms_to_sec(q + 15);
                if (sec > 0) {
                    copy_until(p + 8, ' ', us[us_n].pkg, 96);
                    us[us_n].sec = sec;
                    us_n++;
                }
            }
        }
    }

    /* 各 App 的網路流量：    uid rxBytes rxPackets txBytes txPackets */
    if (nt_state == 0 && starts_with(line, "    uid rxBytes rxPackets txBytes txPackets")) { nt_state = 1; return; }
    if (nt_state == 1) {
        int uid;
        double rx, rp, tx, tp;
        if (sscanf(line, " %d %lf %lf %lf %lf", &uid, &rx, &rp, &tx, &tp) == 5) {
            if (nt_n < 800) { nt[nt_n].uid = uid; nt[nt_n].rx = rx; nt[nt_n].tx = tx; nt_n++; }
        } else {
            nt_state = 2;
        }
    }

    /* 不讓手機休眠的元件 */
    if (wl_state == 0 && starts_with(line, "  All partial wake locks:")) { wl_state = 1; return; }
    if (wl_state == 1) {
        if (starts_with(line, "  Wake lock ")) {
            if (wl_n < 10 && parse_wake(line + 12, 1, &wl[wl_n])) wl_n++;
        } else wl_state = 2;
    }
    /* 叫醒手機的原因 */
    if (wr_state == 0 && starts_with(line, "  All wakeup reasons:")) { wr_state = 1; return; }
    if (wr_state == 1) {
        if (starts_with(line, "  Wakeup reason ")) {
            if (wr_n < 8 && parse_wake(line + 16, 0, &wr[wr_n])) wr_n++;
        } else wr_state = 2;
    }

    /* 權限使用紀錄 */
    if (strcmp(service, "appops") == 0) {
        if (starts_with(line, "  Uid ")) { ap_pkg[0] = '\0'; ap_op = -1; }
        else if (starts_with(line, "    Package ")) { copy_until(line + 12, ':', ap_pkg, sizeof(ap_pkg)); ap_op = -1; }
        else if (starts_with(line, "      ") && isupper((unsigned char)line[6])) {
            ap_op = -1;
            for (i = 0; i < perm_n; i++) {
                a = strlen(perm_op[i]);
                if (strncmp(line + 6, perm_op[i], a) == 0 && line[6 + a] == ' ') { ap_op = perm_cat[i]; break; }
            }
        } else if (ap_op >= 0 && ap_pkg[0] && strcmp(ap_pkg, "android") != 0 && (p = strstr(line, "Access: [")) != NULL && (q = strchr(p, ']')) != NULL) {
            char t[20];
            q++;
            while (*q == ' ') q++;
            if (isdigit((unsigned char)q[0])) {
                strncpy(t, q, 16); t[16] = '\0';           /* 2026-11-18 19:27 */
                for (i = 0; i < pu_n; i++) if (pu[i].cat == ap_op && strcmp(pu[i].pkg, ap_pkg) == 0) break;
                if (i == pu_n && pu_n < 1200) { strcpy(pu[pu_n].pkg, ap_pkg); pu[pu_n].cat = ap_op; pu[pu_n].t[0] = '\0'; pu_n++; }
                if (i < pu_n && strcmp(t, pu[i].t) > 0) strcpy(pu[i].t, t);
            }
        }
    }

    /* 充電紀錄 */
    if (bh_state == 0 && starts_with(line, "Battery History [")) { bh_state = 1; return; }
    if (bh_state == 1) {
        if (line[0] == '\n' || line[0] == '\r' || line[0] == '\0') bh_state = 2;
        else if (line[0] == ' ') bh_line();
    }

    /* 鬧鐘叫醒手機的次數：  u0a92:com.xxx +2m55s running, 47 wakeups: */
    if (strcmp(service, "alarm") == 0 && al_state < 2) {
        if (starts_with(line, "  Alarm Stats:")) al_state = 1;
        else if (al_state == 1) {
            if (starts_with(line, "  ") && line[2] != ' ' && (q = strstr(line, " wakeups:")) != NULL && (p = strchr(line, ':')) != NULL && p < q) {
                char pkg[96];
                long w;
                const char *b = q;
                while (b > line && b[-1] != ' ') b--;
                w = atol(b);
                copy_until(p + 1, ' ', pkg, sizeof(pkg));
                for (i = 0; i < al_n; i++) if (strcmp(al[i].pkg, pkg) == 0) break;
                if (i == al_n && al_n < 300) { strcpy(al[al_n].pkg, pkg); al[al_n].wakes = 0; al_n++; }
                if (i < al_n) al[i].wakes += w;
            } else if (!starts_with(line, "    ")) al_state = 2;
        }
    }

    /* 定位（GNSS） */
    if (strcmp(service, "location") == 0) {
        if (gn_reports < 0 && (p = strstr(line, "Number of location reports: ")) != NULL) gn_reports = atol(p + 28);
        if (gn_fail < 0 && (p = strstr(line, "Percentage location failure: ")) != NULL) gn_fail = atof(p + 29);
        if (gn_ttff_n < 0 && (p = strstr(line, "Number of TTFF reports: ")) != NULL) gn_ttff_n = atol(p + 24);
        if (gn_ttff < 0 && (p = strstr(line, "TTFF mean (sec): ")) != NULL) gn_ttff = atof(p + 17);
        if (gn_acc < 0 && (p = strstr(line, "Position accuracy mean (m): ")) != NULL) gn_acc = atof(p + 28);
        if (gn_cn0 < 0 && starts_with(line, "        Top 4 Avg CN0 mean (dB-Hz): ")) gn_cn0 = atof(line + 36);
        if (gn_l5 < 0 && (p = strstr(line, "Total number of L5 sv status messages processed: ")) != NULL) gn_l5 = atol(p + 49);
        if (gn_const[0] == '\0' && (p = strstr(line, "Used-in-fix constellation types: ")) != NULL) {
            copy_until(p + 33, '\n', gn_const, sizeof(gn_const));
            trim(gn_const);
        }
    }

    /* ---------- 原始資料要遮掉的 Wi-Fi 名稱 ---------- */
    if ((p = strstr(line, "SSID: \"")) != NULL || (p = strstr(line, "wifiNetworkKey=\"")) != NULL) {
        p = strchr(p, '"') + 1;
        copy_until(p, '"', tmp, sizeof(tmp));
        add_mask(tmp);
    }

    /* ---------- 安全檢查 ---------- */
    if (strcmp(service, "settings") == 0 && starts_with(line, "_id:")) read_setting(line);
    if (strcmp(service, "lock_settings") == 0 && sc_lock[0] == '\0' && (p = strstr(line, "CredentialType: ")) != NULL) {
        copy_until(p + 16, '\n', sc_lock, sizeof(sc_lock));
        trim(sc_lock);
    }
    if (sc_face_n < 0 && (p = strstr(line, "{\"service\":\"FaceProvider")) != NULL) read_bio(p, &sc_face_n, &sc_face_ok, &sc_face_ng);
    if (sc_fp_n < 0 && (p = strstr(line, "{\"service\":\"FingerprintProvider")) != NULL) read_bio(p, &sc_fp_n, &sc_fp_ok, &sc_fp_ng);
    if (sc_admin_state == 0 && starts_with(line, "  Enabled Device Admins (User 0")) { sc_admin_state = 1; return; }
    if (sc_admin_state == 1) {
        if (starts_with(line, "    ") && line[4] != ' ' && (p = strchr(line, '/')) != NULL && strstr(line, "mPassword") == NULL) {
            if (sc_admin_n < 8) { copy_until(line + 4, '/', sc_admin[sc_admin_n], 96); sc_admin_n++; }
        } else if (!starts_with(line, "      ") && !(line[0] == ' ' && line[1] == ' ' && line[2] == '\n')) {
            sc_admin_state = 2;
        }
    }
    /* 可以安裝未知來源 App（appops 的 REQUEST_INSTALL_PACKAGES (allow)） */
    if (strcmp(service, "appops") == 0 && ap_pkg[0] && starts_with(line, "      REQUEST_INSTALL_PACKAGES (allow)")) {
        for (i = 0; i < sc_install_n; i++) if (strcmp(sc_install[i], ap_pkg) == 0) break;
        if (i == sc_install_n && sc_install_n < 40) strcpy(sc_install[sc_install_n++], ap_pkg);
    }

    /* ---------- 設定狀態：不受電池最佳化限制的 App ---------- */
    if (st_white_state == 0 && starts_with(line, "  Whitelist user apps:")) { st_white_state = 1; return; }
    if (st_white_state == 1) {
        if (starts_with(line, "    ") && line[4] != ' ') {
            if (st_white_n < 60) { copy_until(line + 4, ' ', st_white[st_white_n], 64); trim(st_white[st_white_n]); st_white_n++; }
        } else st_white_state = 2;
    }

    /* ---------- 效能 ---------- */
    if (pm_state == 0 && (starts_with(line, "Total PSS by process:") || starts_with(line, "Total RSS by process:"))) {
        pm_state = 1;
        copy_until(line + 6, ' ', pm_title, sizeof(pm_title));     /* PSS 或 RSS */
        return;
    }
    if (pm_state == 1) {
        if (starts_with(line, "    ") && (p = strstr(line, "K: ")) != NULL) {
            if (pm_n < 10) {
                pm[pm_n].val = kb_num(line);
                copy_until(p + 3, ' ', pm[pm_n].name, 64);
                pm_n++;
            }
        } else pm_state = 2;
    }
    if (strcmp(section, "MEMORY INFO") != 0 && mi_total < 0 && starts_with(line, "Total RAM: ")) {
        mi_total = (long)kb_num(line + 11);
        if ((p = strstr(line, "(status ")) != NULL) copy_until(p + 8, ')', mi_status, sizeof(mi_status));
    }
    if (mi_free < 0 && starts_with(line, " Free RAM: ")) mi_free = (long)kb_num(line + 11);
    if (mi_used < 0 && starts_with(line, " Used RAM: ")) mi_used = (long)kb_num(line + 11);
    if (mi_lost < 0 && starts_with(line, " Lost RAM: ")) mi_lost = (long)kb_num(line + 11);
    if (mi_zram < 0 && starts_with(line, "     ZRAM: ")) {
        mi_zram = (long)kb_num(line + 11);
        if ((p = strstr(line, "used for ")) != NULL) mi_swap_used = (long)kb_num(p + 9);
    }
    if (strstr(service, "cpuinfo") != NULL && pc_state < 2) {
        if (pc_load[0] == '\0' && starts_with(line, "Load: ")) copy_until(line + 6, '\n', pc_load, sizeof(pc_load));
        if (starts_with(line, "CPU usage from ")) { pc_state = 1; return; }
        if (pc_state == 1) {
            if (line[0] == ' ' && (p = strchr(line, '%')) != NULL && (q = strchr(p, '/')) != NULL && isdigit((unsigned char)p[2])) {
                if (pc_n < 10) {
                    pc[pc_n].val = atof(line);
                    copy_until(q + 1, ':', pc[pc_n].name, 64);
                    pc_n++;
                }
            } else pc_state = 2;
        }
    }

    /* ---------- 通知與相機 ---------- */
    if (starts_with(line, "    NotificationRecord(0x") && (p = strstr(line, " pkg=")) != NULL && strstr(service, "notification") != NULL) {
        copy_until(p + 5, ' ', tmp, sizeof(tmp));
        add_count(nf, &nf_n, 200, tmp);
    }
    if (strcmp(service, "media.camera") == 0 && (p = strstr(line, " : CONNECT device ")) != NULL && (q = strstr(p, "for package ")) != NULL) {
        copy_until(q + 12, ' ', tmp, sizeof(tmp));
        if (!starts_with(tmp, "client.pid")) add_count(cam, &cam_n, 60, tmp);
    }

    /* ---------- 進階數據 ---------- */
    if (kw_state == 0 && starts_with(line, "  All kernel wake locks:")) { kw_state = 1; return; }
    if (kw_state == 1) {
        if (starts_with(line, "  Kernel Wake lock ")) {
            if (kw_n < 10 && parse_wake(line + 19, 0, &kw[kw_n])) kw_n++;
        } else kw_state = 2;
    }
    if (strcmp(service, "thermalservice") == 0 && tz_state < 2) {
        if (starts_with(line, "Cached temperatures:")) { tz_state = 1; return; }
        if (tz_state == 1) {
            if (line[0] == '\t' && (p = strstr(line, "mValue=")) != NULL && (q = strstr(line, "mName=")) != NULL) {
                double v = atof(p + 7);
                if (tz_n < 40 && v > 0 && v < 150) {
                    copy_until(q + 6, ',', tz[tz_n].name, 24);
                    tz[tz_n].v = v;
                    tz_n++;
                }
            } else tz_state = 2;
        }
    }

    /* 核心版本：Kernel: Linux version 6.6.118-android15-8-... */
    if (x_kernel[0] == '\0' && starts_with(line, "Kernel: Linux version ")) {
        copy_until(line + 22, ' ', x_kernel, sizeof(x_kernel));
        if ((p = strstr(x_kernel, "-g")) != NULL) *p = '\0';      /* 後面是程式碼版本的編號，不用看 */
        if ((p = strstr(x_kernel, "-ab")) != NULL) *p = '\0';
    }

    /* 目前電池狀態 */
    if (strcmp(service, "battery") == 0) {
        if (xb_level < 0 && starts_with(line, "  level: ")) xb_level = atoi(line + 9);
        if (xb_volt < 0 && starts_with(line, "  voltage: ")) xb_volt = atoi(line + 11);
        if (xb_temp == -9999 && starts_with(line, "  temperature: ")) xb_temp = atoi(line + 15);
        if (xb_status < 0 && starts_with(line, "  status: ")) xb_status = atoi(line + 10);
        if (xb_health < 0 && starts_with(line, "  health: ")) xb_health = atoi(line + 10);
        if (xb_counter < 0 && starts_with(line, "  Charge counter: ")) xb_counter = atol(line + 18);
        if (xb_tech[0] == '\0' && starts_with(line, "  technology: ")) { copy_until(line + 14, '\n', xb_tech, sizeof(xb_tech)); trim(xb_tech); }
        if (starts_with(line, "  AC powered: true")) xb_ac = 1;
        if (starts_with(line, "  USB powered: true")) xb_usb = 1;
        if (starts_with(line, "  Wireless powered: true")) xb_wl = 1;
    }

    /* 耗電排行 */
    if (xp_state == 0 && strstr(line, "Estimated power use (mAh):") != NULL) { xp_state = 1; return; }
    if (xp_state == 1) {
        if (line[0] != ' ') xp_state = 2;
        else if (xp_capacity < 0 && (p = strstr(line, "Capacity: ")) != NULL && isdigit((unsigned char)p[10])) xp_capacity = atol(p + 10);
        else if (starts_with(line, "  UID ") && xp_n < 400 && (p = strchr(line + 6, ':')) != NULL) {
            a = p - (line + 6);
            if (a > 15) a = 15;
            strncpy(xp[xp_n].uid, line + 6, a);
            xp[xp_n].uid[a] = '\0';
            xp[xp_n].mah = atof(p + 1);
            xp_n++;
        }
    }
    if (xp_onbatt[0] == '\0' && starts_with(line, "  Time on battery: ")) copy_until(line + 19, '(', xp_onbatt, sizeof(xp_onbatt));
    if (xp_screen[0] == '\0' && starts_with(line, "  Screen on: ")) copy_until(line + 13, '(', xp_screen, sizeof(xp_screen));

    /* 套件：名稱、uid、版本、安裝 / 更新時間、是不是系統內建 */
    if (strcmp(service, "package") == 0) {
        if (starts_with(line, "  Package [")) {
            char nm[96];
            copy_until(line + 11, ']', nm, sizeof(nm));
            pk_wait = -1;
            for (i = 0; i < pk_n; i++) if (strcmp(pk_name[i], nm) == 0) break;
            if (i == pk_n && pk_n < 1500) {          /* 同一個套件後面又出現（舊版本）就不管 */
                strcpy(pk_name[pk_n], nm);
                pk_uid[pk_n] = -1;
                pk_ver[pk_n][0] = pk_first[pk_n][0] = pk_last[pk_n][0] = '\0';
                pk_sys[pk_n] = 0;
                pk_wait = pk_n++;
            }
        } else if (starts_with(line, "  ") && line[2] != ' ') {
            pk_wait = -1;
        } else if (pk_wait >= 0) {
            if (pk_uid[pk_wait] < 0 && ((p = strstr(line, "appId=")) != NULL || (p = strstr(line, "userId=")) != NULL))
                pk_uid[pk_wait] = atoi(strchr(p, '=') + 1);
            if (pk_ver[pk_wait][0] == '\0' && starts_with(line, "    versionName=")) copy_until(line + 16, ' ', pk_ver[pk_wait], 24);
            if (pk_last[pk_wait][0] == '\0' && starts_with(line, "    lastUpdateTime=")) copy_until(line + 19, '\n', pk_last[pk_wait], 20);
            if (pk_first[pk_wait][0] == '\0' && (p = strstr(line, "firstInstallTime=")) != NULL) copy_until(p + 17, '\n', pk_first[pk_wait], 20);
            if (starts_with(line, "    pkgFlags=[") && strstr(line, " SYSTEM ") != NULL) pk_sys[pk_wait] = 1;
        }
    }

    /* 儲存空間用量 */
    if (strcmp(service, "diskstats") == 0) {
        if (starts_with(line, "App Size: ")) xd_app = atof(line + 10);
        else if (starts_with(line, "App Data Size: ")) xd_data = atof(line + 15);
        else if (starts_with(line, "App Cache Size: ")) xd_cache = atof(line + 16);
        else if (starts_with(line, "Photos Size: ")) xd_photo = atof(line + 13);
        else if (starts_with(line, "Videos Size: ")) xd_video = atof(line + 13);
        else if (starts_with(line, "Audio Size: ")) xd_audio = atof(line + 12);
        else if (starts_with(line, "Other Size: ")) xd_other = atof(line + 12);
        else if (starts_with(line, "Recent Disk Write Speed (kB/s) = ")) xd_write = atol(line + 33);
        else if (xd_free_kb < 0 && starts_with(line, "Data-Free: ")) xd_free_kb = atol(line + 11);
        else if (starts_with(line, "Package Names: ")) read_name_array(line + 15);
        else if (starts_with(line, "App Sizes: ") || starts_with(line, "App Data Sizes: ")) add_size_array(line);
    }

    /* 溫度 */
    if (strcmp(service, "thermalservice") == 0 && !xt_done) {
        if (xt_status < 0 && starts_with(line, "Thermal Status: ")) xt_status = atoi(line + 16);
        if (starts_with(line, "Current temperatures from HAL:")) { xt_inhal = 1; return; }
        if (xt_inhal) {
            double v;
            int t;
            if (line[0] == '\t' && (p = strstr(line, "mValue=")) != NULL && (q = strstr(line, "mType=")) != NULL) {
                v = atof(p + 7);
                t = atoi(q + 6);
                if (t >= 0 && t < 16 && v > 0 && v < 150 && v > xt[t]) xt[t] = v;
            } else {
                xt_inhal = 0;
                xt_done = 1;
            }
        }
    }

    /* 感測器 */
    if (strstr(service, "sensorservice") != NULL && xs_state < 2) {
        if (starts_with(line, "Sensor List:")) { xs_state = 1; return; }
        if (xs_state == 1) {
            if (starts_with(line, "0x") && (p = strstr(line, ") ")) != NULL) {
                char name[64], vendor[24];
                int t = -1;
                xs_total++;
                copy_until(p + 2, '|', name, sizeof(name));
                trim(name);
                q = strstr(name, " Non-wakeup"); if (q != NULL) *q = '\0';
                q = strstr(name, " Wakeup");     if (q != NULL) *q = '\0';
                for (i = 0, a = 0; name[i]; i++) {          /* 連續的空白只留一個 */
                    if (name[i] == ' ' && a > 0 && name[a - 1] == ' ') continue;
                    name[a++] = name[i];
                }
                name[a] = '\0';
                trim(name);
                vendor[0] = '\0';
                q = strchr(p + 2, '|');
                if (q != NULL) {
                    q++;
                    while (*q == ' ') q++;
                    copy_until(q, '|', vendor, sizeof(vendor));
                    trim(vendor);
                }
                q = strstr(line, "type: ");
                if (q != NULL && (q = strchr(q, '(')) != NULL) t = atoi(q + 1);
                if (sensor_cn(t) != NULL && xs_n < 16) {
                    for (i = 0; i < xs_n; i++) if (xs[i].type == t) break;
                    if (i == xs_n) {
                        xs[xs_n].type = t;
                        strcpy(xs[xs_n].name, name);
                        strcpy(xs[xs_n].vendor, vendor);
                        xs_n++;
                    }
                }
            } else if (line[0] != '\t') {
                xs_state = 2;
            }
        }
    }

    /* 螢幕細節 */
    if (xdp_hdr[0] == '\0' && (p = strstr(line, "mSupportedHdrTypes=[")) != NULL) {
        p += 20;
        tmp[0] = '\0';
        while (*p && *p != ']') {
            a = atoi(p);
            if (tmp[0]) strcat(tmp, "、");
            if (a == 1) strcat(tmp, "Dolby Vision");
            else if (a == 2) strcat(tmp, "HDR10");
            else if (a == 3) strcat(tmp, "HLG");
            else if (a == 4) strcat(tmp, "HDR10+");
            while (*p && *p != ',' && *p != ']') p++;
            if (*p == ',') p++;
            while (*p == ' ') p++;
        }
        strcpy(xdp_hdr, tmp[0] ? tmp : "不支援");
    }
    /* 「density 520, 447.02 x 447.02 dpi」或「density 520 (447.02 x 447.02) dpi」 */
    if (xdp_density == 0 && (p = strstr(line, " density ")) != NULL && isdigit((unsigned char)p[9])) {
        q = p + 9;
        while (isdigit((unsigned char)*q)) q++;
        if ((q[0] == ',' && q[1] == ' ') || (q[0] == ' ' && q[1] == '(')) {
            xdp_density = atoi(p + 9);
            xdp_dpi = atof(q + 2);
        }
    }
    if (xdp_nrates == 0 && (p = strstr(line, "supportedRefreshRates [")) != NULL) {
        p += 23;
        while (*p && *p != ']') {
            double r = atof(p);
            int dup = 0;
            for (i = 0; i < xdp_nrates; i++) if (xdp_rates[i] > r - 0.5 && xdp_rates[i] < r + 0.5) dup = 1;
            if (!dup && r > 0 && xdp_nrates < 16) xdp_rates[xdp_nrates++] = r;
            while (*p && *p != ',' && *p != ']') p++;
            if (*p == ',') p++;
            while (*p == ' ') p++;
        }
    }

    /* 目前的 Wi-Fi 連線 */
    if (xw_link < 0 && (p = strstr(line, "mWifiInfo SSID: ")) != NULL) {
        if ((q = strstr(p, "Wi-Fi standard: ")) != NULL) copy_until(q + 16, ',', xw_std, sizeof(xw_std));
        if ((q = strstr(p, "RSSI: ")) != NULL) xw_rssi = atoi(q + 6);
        if ((q = strstr(p, "Link speed: ")) != NULL) xw_link = atoi(q + 12);
        if ((q = strstr(p, "Frequency: ")) != NULL) xw_freq = atoi(q + 11);
        if (xw_link < 0) xw_link = 0;
    }

    /* 記憶體目前的狀態 */
    if (strcmp(section, "MEMORY INFO") == 0) {
        if (xm_avail < 0 && starts_with(line, "MemAvailable:")) xm_avail = atol(line + 13);
        if (xm_swap_total < 0 && starts_with(line, "SwapTotal:")) xm_swap_total = atol(line + 10);
        if (xm_swap_free < 0 && starts_with(line, "SwapFree:")) xm_swap_free = atol(line + 9);
    }

    /* 藍牙 */
    if (strcmp(service, "bluetooth_manager") == 0) {
        if (xbt_state[0] == '\0' && (starts_with(line, "  State:") || starts_with(line, "  state:"))) {
            p = line + 8;
            while (*p == ' ') p++;
            copy_until(p, '\n', xbt_state, sizeof(xbt_state));
            trim(xbt_state);
        }
        if (xbt_name[0] == '\0' && (starts_with(line, "  Name:") || starts_with(line, "  name:"))) {
            p = line + 7;
            while (*p == ' ') p++;
            copy_until(p, '\n', xbt_name, sizeof(xbt_name));
            trim(xbt_name);
        }
        if (xbt_bonded < 0 && (p = strstr(line, "Bonded devices:")) != NULL) {
            p += 15;
            while (*p == ' ') p++;
            xbt_bonded = isdigit((unsigned char)*p) ? atoi(p) : 0;
            xbt_in_bonded = 1;
            return;
        }
        if (xbt_in_bonded) {
            /*     XX:XX:XX:XX:A9:AA [ DUAL ][ 0x001f00 ] Xiaomi Smart Band 9 A9AA (SPP,...) */
            if (starts_with(line, "    ") && (p = strstr(line, "][ 0x")) != NULL && (q = strchr(p + 2, ']')) != NULL) {
                if (xbt_ndev < 20) {
                    p = q + 1;
                    while (*p == ' ') p++;
                    copy_until(p, '\n', xbt_dev[xbt_ndev], 64);
                    q = strstr(xbt_dev[xbt_ndev], " (");
                    if (q != NULL) *q = '\0';
                    trim(xbt_dev[xbt_ndev]);
                    xbt_ndev++;
                }
            } else if (line[0] == '\n' || line[0] == '\r' || (starts_with(line, "  ") && isalpha((unsigned char)line[2]))) {
                xbt_in_bonded = 0;
            }
        }
    }

    /* 喇叭擴大機：看真的在動作的晶片（核心紀錄），不是驅動清單 */
    if ((p = strstr(line, "cs35l")) != NULL && isdigit((unsigned char)p[5]) && isdigit((unsigned char)p[6]) && p[7] == ' '
        && isdigit((unsigned char)p[8]) && (q = strstr(p + 8, "-00")) != NULL && q - p < 12) {
        char addr[16];
        if (xa_chip[0] == '\0') sprintf(xa_chip, "CS35L%c%c", p[5], p[6]);
        copy_until(p + 8, ':', addr, sizeof(addr));
        for (i = 0; i < xa_naddr; i++) if (strcmp(xa_addr[i], addr) == 0) break;
        if (i == xa_naddr && xa_naddr < 8) strcpy(xa_addr[xa_naddr++], addr);
    }
    if ((p = strstr(line, "[smartpa-codec_")) != NULL && isdigit((unsigned char)p[15])) {
        a = atoi(p + 15) + 1;
        if (a > xa_aw) xa_aw = a;
        if (xa_chip[0] == '\0' && strstr(line, "aw882xx") != NULL) strcpy(xa_chip, "AW882xx");
    }
    if (xa_chip[0] == '\0' && (strstr(line, "[sipa_91xx]") != NULL || strstr(line, "sia91xx_") != NULL)) strcpy(xa_chip, "SIA91xx");
    if (strstr(line, "speaker type is stereo") != NULL) xa_stereo = 1;

    /* 震動馬達 */
    if (xa_haptic[0] == '\0') {
        if (strstr(line, "cs40l26") != NULL) strcpy(xa_haptic, "Cirrus Logic CS40L26");
        else if ((p = strstr(line, "aw869")) != NULL && isdigit((unsigned char)p[5]) && isdigit((unsigned char)p[6])) {
            copy_until(p, ' ', tmp, 16);
            for (i = 0; tmp[i]; i++) tmp[i] = toupper((unsigned char)tmp[i]);
            sprintf(xa_haptic, "艾為 Awinic %s", tmp);
        }
    }

    if (xa_haptic[0] == '\0' && strcmp(section, "LSMOD") == 0 && starts_with(line, "aw8697 ")) strcpy(xa_haptic, "艾為 Awinic AW8697 (驅動清單)");

    /* 三星的鏡頭：CAM_ACQUIRE_DEV Success for s5kgh1 sensor_id:... */
    if ((p = strstr(line, "CAM_ACQUIRE_DEV Success for ")) != NULL) {
        copy_until(p + 28, ' ', tmp, 16);
        add_cam(tmp);
    }

    /* 指紋晶片：三星 type:QBT2608、vivo soc:qcom_qfs2630 */
    if (x_fp[0] == '\0') {
        if ((p = strstr(line, "type:QBT")) != NULL) copy_until(p + 5, ',', x_fp, sizeof(x_fp));
        else if ((p = strstr(line, "qcom_qfs")) != NULL && isdigit((unsigned char)p[8])) {
            copy_until(p + 5, ',', x_fp, sizeof(x_fp));
            for (i = 0; x_fp[i]; i++) x_fp[i] = toupper((unsigned char)x_fp[i]);
        }
    }
}

/* ---------------- 讀完之後整理 ---------------- */

void ex_finish(void)
{
    if (xbt_name[0]) add_mask(xbt_name);
    if (bh_charging && bh_prev >= 0) {        /* 抓報告的時候還在充電 */
        bh_end_session(bh_last_t, bh_lastst, bh_prev);
    }
    if (g_fp_text[0] == '\0' && x_fp[0]) sprintf(g_fp_text, "高通 Qualcomm 超聲波 (%s)", x_fp);
    if (xa_naddr >= 2 || xa_aw >= 2) xa_stereo = 1;
}

/* ##################################################################
 * 7. 主程式
 * ################################################################## */

/* ================= 第 1 部分：選檔案 ================= */

int select_file(char *path)
{
    double choice;
    const char *finder;
    char dir[1000];
    char names[500][260];
    time_t times[500];
    double sizes[500];
    int isdir[500];
    int count, num, i, j;
    char tmpname[260];
    time_t tmptime;
    double tmpsize;
    int tmpdir;
    char date[30], sizestr[30], ans;
    DIR *d;
    struct dirent *e;
    struct stat info;

    while (1) {
        printf("1.桌面\n2.下載\n3.文件\n請輸入路徑代碼\n");
        scanf("%lf", &choice);
        if (choice == 1) { finder = "Desktop"; break; }
        else if (choice == 2) { finder = "Downloads"; break; }
        else if (choice == 3) { finder = "Documents"; break; }
        else { printf("找不到此選項,請重新輸入\n\n"); }
    }
    sprintf(dir, "%s\\%s", getenv("USERPROFILE"), finder);

    while (1) {
        d = opendir(dir);
        if (d == NULL) {
            printf("無法打開資料夾:%s\n", dir);
            return 0;
        }
        count = 0;
        while ((e = readdir(d)) != NULL && count < 500) {
            if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0) continue;
            sprintf(path, "%s\\%s", dir, e->d_name);
            if (stat(path, &info) != 0) continue;
            strcpy(names[count], e->d_name);
            times[count] = info.st_mtime;
            sizes[count] = (double)info.st_size;
            isdir[count] = S_ISDIR(info.st_mode);
            count++;
        }
        closedir(d);

        /* 照時間排序，新的在前 */
        for (i = 0; i < count - 1; i++) {
            for (j = 0; j < count - 1 - i; j++) {
                if (times[j] < times[j+1]) {
                    strcpy(tmpname, names[j]); strcpy(names[j], names[j+1]); strcpy(names[j+1], tmpname);
                    tmptime = times[j]; times[j] = times[j+1]; times[j+1] = tmptime;
                    tmpsize = sizes[j]; sizes[j] = sizes[j+1]; sizes[j+1] = tmpsize;
                    tmpdir = isdir[j]; isdir[j] = isdir[j+1]; isdir[j+1] = tmpdir;
                }
            }
        }

        printf("\n目前位置:%s\n", dir);
        printf("  0. 回上一層\n");
        for (i = 0; i < count; i++) {
            strftime(date, sizeof(date), "%Y/%m/%d %H:%M", localtime(&times[i]));
            if (isdir[i]) {
                printf("%3d. %-45s %s   [資料夾]\n", i + 1, names[i], date);
            } else {
                if (sizes[i] >= 1024.0 * 1024 * 1024) sprintf(sizestr, "%.1f GB", sizes[i] / (1024.0 * 1024 * 1024));
                else if (sizes[i] >= 1024.0 * 1024) sprintf(sizestr, "%.1f MB", sizes[i] / (1024.0 * 1024));
                else if (sizes[i] >= 1024.0)        sprintf(sizestr, "%.1f KB", sizes[i] / 1024.0);
                else                                sprintf(sizestr, "%.0f B", sizes[i]);
                printf("%3d. %-45s %s %10s\n", i + 1, names[i], date, sizestr);
            }
        }

        printf("請輸入編號:");
        scanf("%d", &num);
        if (num == 0) {
            char *p = strrchr(dir, '\\');
            if (p != NULL) *p = '\0';
            continue;
        }
        if (num < 1 || num > count) {
            printf("沒有這個編號,請重新輸入\n");
            continue;
        }
        sprintf(path, "%s\\%s", dir, names[num-1]);
        if (isdir[num-1]) {
            strcpy(dir, path);
            continue;
        }
        printf("你選了:%s\n確定嗎?(y/n)", names[num-1]);
        scanf(" %c", &ans);
        if (ans == 'y' || ans == 'Y') return 1;
    }
}

/* ================= 第 2 部分：一行一行讀，順便抓資料 ================= */

/* App 被關的紀錄：am_kill: [user,pid,程式,adj,原因,記憶體] */
void handle_kill(const char *s)
{
    const char *p = strchr(s, '[');
    const char *end = strrchr(s, ']');
    const char *c1, *c2, *c3, *c4, *last;
    char proc[96], reason[160];
    int i, len;

    if (p == NULL || end == NULL) return;
    c1 = strchr(p + 1, ',');   if (c1 == NULL) return;
    c2 = strchr(c1 + 1, ',');  if (c2 == NULL) return;
    c3 = strchr(c2 + 1, ',');  if (c3 == NULL) return;
    c4 = strchr(c3 + 1, ',');  if (c4 == NULL) return;
    last = end;
    while (last > c4 && *last != ',') last--;
    if (last <= c4) return;

    len = c3 - (c2 + 1);
    if (len > 95) len = 95;
    strncpy(proc, c2 + 1, len); proc[len] = '\0';
    len = last - (c4 + 1);
    if (len > 159) len = 159;
    strncpy(reason, c4 + 1, len); reason[len] = '\0';

    for (i = 0; i < kill_n; i++) {
        if (strstr(c4 + 1, kill_key[i]) != NULL) break;     /* 從原因開始找到最後（OPPO 的格式多一欄） */
    }
    if (i < kill_n && kill_routine[i]) return;      /* 常規原因，不列出 */
    if (g_nkill >= MAX_LIST) return;

    strncpy(kills[g_nkill].time, s, 11); kills[g_nkill].time[11] = '\0';
    strcpy(kills[g_nkill].proc, proc);
    if (i == kill_n) {
        strcpy(kills[g_nkill].reason, "其他原因（尚未收錄）");
        if (g_nunknown < MAX_LIST) strcpy(unknown_reason[g_nunknown++], reason);
    } else if (i == 6) {
        /* 連帶關閉：找出它依賴的是誰 */
        char dep[64] = "";
        const char *q = strstr(reason, "in dying proc ");
        if (q != NULL) copy_until(q + 14, ' ', dep, sizeof(dep));
        sprintf(kills[g_nkill].reason, "它依賴的 %s 正在關閉，被連帶關掉", dep);
    } else {
        strcpy(kills[g_nkill].reason, kill_cn[i]);
        if (kill_bad[i]) g_kill_bad++;
    }
    g_nkill++;
}

/* 錯誤訊息：E / F 等級，不在黑名單就寫進 important.txt */
void handle_error_line(const char *s)
{
    char lv[8], tag[64];
    int i;
    if (sscanf(s, "%*s %*s %*s %*s %*s %7s %63[^:]", lv, tag) != 2) return;
    if (strlen(lv) != 1) return;
    if (lv[0] != 'E' && lv[0] != 'F') return;
    trim(tag);
    if (lv[0] == 'E') g_e++; else g_f++;
    for (i = 0; i < blacklist_n; i++) {
        if (strcmp(tag, blacklist[i]) == 0) { g_black++; return; }
    }
    g_kept++;
    if (imp != NULL) fputs(s, imp);
}

/* 讀取進度（手機 App 用來顯示進度條）：已經讀了幾個位元組、檔案總共幾個位元組 */
volatile double g_prog_done = 0, g_prog_total = 0;

long read_file(const char *path)
{
    FILE *in;
    long total = 0;
    char section[64] = "", service[64] = "";
    char tmp[128];
    char *p;
    int a, i;
    long la, lb;
    clock_t start = clock();

    /* 一定要用 "rb"：檔案中間有二進位資料，用 "r" 會讀到一半就停 */
    in = fopen(path, "rb");
    if (in == NULL) return -1;

    printf("\n正在讀取... 這個檔案很大，請稍等\n");
    fseek(in, 0, SEEK_END);
    g_prog_total = (double)ftell(in);
    rs_file_size = ftell(in);
    strncpy(rs_path, path, sizeof(rs_path) - 1);
    fseek(in, 0, SEEK_SET);

    while (fgets(line, LINE_SIZE, in) != NULL) {
        total++;
        if (total % 2000 == 0) g_prog_done = (double)ftell(in);
        if (total % 500000 == 0) printf("  已讀 %ld 行\n", total);

        /* ---- 區段標題 ---- */
        if (starts_with(line, "------ ") && strstr(line, "was the duration") == NULL) {
            i = 0;
            p = line + 7;
            while (p[i] != '\0' && p[i] != '(' && p[i] != '\n' && !starts_with(p + i, " ------") && i < 63) {
                section[i] = p[i];
                i++;
            }
            section[i] = '\0';
            trim(section);
            service[0] = '\0';
            raw_add(line + 7, 'S', ftell(in));      /* 原始資料用：記下這個區段從哪裡開始 */
            g_wifi_score_on = 0;
            if (strcmp(section, "VM TRACES AT LAST ANR") == 0) {
                p = strstr(line, "/data/anr/");
                if (p != NULL) copy_until(p + 10, ':', g_last_anr_file, sizeof(g_last_anr_file));
                g_wait_subject = 1;
            }
            continue;
        }
        if (starts_with(line, "DUMP OF SERVICE ")) {
            copy_until(line + 16, ':', service, sizeof(service));
            raw_add(line + 16, 'D', ftell(in));
            g_wifi_score_on = 0;
            continue;
        }

        /* ---- 檔案開頭：抓取時間 ---- */
        if (g_capture[0] == '\0' && starts_with(line, "== dumpstate: ")) {
            copy_until(line + 14, '\n', g_capture, sizeof(g_capture));
            sscanf(g_capture, "%d", &g_cap_year);
        }

        /* ---- 各廠商自己的讀法 ---- */
        android_line(section, service);
        xiaomi_line(section, service);       /* 廠商 */
        samsung_line(section, service);      /* 廠商 */
        oppo_line(section, service);         /* 廠商 */
        vivo_line(section, service);         /* 廠商 */
        ex_line(section, service);           /* 更多資料（每家都有） */

        /* ---- 系統參數 ---- */
        if (line[0] == '[' && strcmp(section, "SYSTEM PROPERTIES") == 0) {
            get_prop(line, "ro.product.manufacturer", g_maker, sizeof(g_maker));
            get_prop(line, "ro.product.marketname", g_model, sizeof(g_model));
            get_prop(line, "ro.product.model", g_model_code, sizeof(g_model_code));
            get_prop(line, "ro.build.version.release", g_android, sizeof(g_android));
            get_prop(line, "ro.build.version.security_patch", g_patch, sizeof(g_patch));
            get_prop(line, "ro.boot.verifiedbootstate", g_verified, sizeof(g_verified));
            get_prop(line, "ro.boot.flash.locked", g_locked, sizeof(g_locked));
            get_prop(line, "ro.soc.model", g_soc, sizeof(g_soc));
            get_prop(line, "ro.soc.manufacturer", g_soc_maker, sizeof(g_soc_maker));
            continue;
        }

        /* ---- 開機時間 ---- */
        if (strcmp(section, "UPTIME") == 0 && (p = strstr(line, " up ")) != NULL) {
            int dd = 0, hh = 0, mm = 0;
            char *q;
            if (strstr(line, " day") != NULL) {
                /* 例如 up 1 day, 23:41 */
                sscanf(p, " up %d", &dd);
                q = strchr(p, ',');
                if (q != NULL) sscanf(q + 1, "%d:%d", &hh, &mm);
                sprintf(g_uptime, "%d 天 %d 小時 %d 分", dd, hh, mm);
            } else if (sscanf(p, " up %d:%d", &hh, &mm) == 2) {
                sprintf(g_uptime, "%d 小時 %d 分", hh, mm);
            } else if (sscanf(p, " up %d", &mm) == 1) {
                sprintf(g_uptime, "%d 分", mm);
            }
        }

        /* ---- 記憶體、處理器、儲存容量 ---- */
        if (strcmp(section, "MEMORY INFO") == 0 && starts_with(line, "MemTotal:")) sscanf(line + 9, "%ld", &g_memtotal_kb);
        if (strcmp(section, "CPU INFO") == 0 && g_cores == 0 && strstr(line, "%cpu ") != NULL) {
            if (sscanf(line, "%d%%cpu", &a) == 1) g_cores = a / 100;
        }
        if (strcmp(section, "KERNEL CPUFREQ") == 0 && sscanf(line, "%ld %ld", &la, &lb) == 2) {
            g_time_total += lb;
            if (g_freq_max == 0 || la > g_freq_max) g_freq_max = la;
            /* 最低頻率只算「真的有跑過」的，有些頻率完全不用 */
            if (lb > 0 && (g_freq_min == 0 || la < g_freq_min)) { g_freq_min = la; g_time_min = lb; }
        }
        if (g_data_total_kb == 0 && (p = strstr(line, "Data-Free: ")) != NULL && (p = strstr(p, " / ")) != NULL) {
            sscanf(p + 3, "%ld", &g_data_total_kb);
        }

        /* ---- 觸控晶片：中斷清單裡最後一個字是 xxx_ts 的 ---- */
        if (strcmp(section, "INTERRUPTS") == 0 && g_touch[0] == '\0') {
            char last[64] = "", *tok;
            tok = strrchr(line, ' ');
            if (tok != NULL) {
                copy_until(tok + 1, '\n', last, sizeof(last));
                trim(last);
                a = strlen(last);
                if (a > 3 && strcmp(last + a - 3, "_ts") == 0) strcpy(g_touch, last);
            }
        }

        /* ---- 驅動程式清單：充電、喇叭、Wi-Fi 晶片 ---- */
        if (strcmp(section, "LSMOD") == 0) {
            char mod[64];
            int n;
            if (sscanf(line, "%63s", mod) == 1) {
                n = strlen(mod);
                if (g_ncharger < 4 && !starts_with(mod, "mtk_") && !starts_with(mod, "charger") &&
                    ((n > 4 && strcmp(mod + n - 4, "_chg") == 0) || (n > 8 && strcmp(mod + n - 8, "_charger") == 0))) {
                    strcpy(g_chargers[g_ncharger++], mod);
                }
                if (g_amp[0] == '\0' && starts_with(mod, "snd_soc_") &&
                    (starts_with(mod + 8, "cs35") || starts_with(mod + 8, "aw8") || starts_with(mod + 8, "tfa"))) {
                    strcpy(g_amp, mod + 8);
                }
                if (g_wifi_chip[0] == '\0' && starts_with(mod, "wlan_drv_")) {
                    p = strrchr(mod, '_');
                    sprintf(g_wifi_chip, "MT%s", p + 1);
                }
            }
        }

        /* ---- 觸控晶片（輸入裝置清單） ---- */
        if (g_touch[0] == '\0' && strstr(service, "input") != NULL) {
            char dev[64];
            if (sscanf(line, " %d: %63s", &a, dev) == 2 && (strstr(dev, "_ts") != NULL || strstr(dev, "touch") != NULL)) {
                strcpy(g_touch, dev);
            }
        }

        /* ---- 鏡頭、GPU ---- */
        if ((p = strstr(line, "sensorName (")) != NULL) {
            char list[64], *tok;
            copy_until(p + 12, ')', list, sizeof(list));
            tok = strtok(list, ";");
            while (tok != NULL) {
                if (!isupper((unsigned char)tok[0])) { tok = strtok(NULL, ";"); continue; }
                for (i = 0; i < g_ncam; i++) if (strcmp(g_cams[i], tok) == 0) break;
                if (i == g_ncam && g_ncam < 8) { strncpy(g_cams[g_ncam], tok, 15); g_cams[g_ncam][15] = '\0'; g_ncam++; }
                tok = strtok(NULL, ";");
            }
        }
        if (g_gpu[0] == '\0' && (p = strstr(line, "Adreno (TM) ")) != NULL && isdigit((unsigned char)p[12])) {
            sprintf(g_gpu, "Adreno %d", atoi(p + 12));
        }
        if (g_gpu[0] == '\0' && (p = strstr(line, "Mali-")) != NULL) {
            i = 0;
            while ((isalnum((unsigned char)p[i]) || p[i] == '-') && i < 47) { g_gpu[i] = p[i]; i++; }
            g_gpu[i] = '\0';
        }

        /* ---- 螢幕 ---- */
        if (g_w == 0 && (p = strstr(line, "supportedRefreshRates [")) != NULL) {
            char *q;
            sscanf(p + 23, "%lf", &g_max_hz);
            q = strstr(line, "uniqueId=");
            if (q != NULL && (q = strstr(q, " x ")) != NULL) {
                while (q > line && isdigit((unsigned char)q[-1])) q--;
                sscanf(q, "%d x %d", &g_w, &g_h);
            }
        }

        /* ---- 系統設定 ---- */
        if (strcmp(service, "settings") == 0 && starts_with(line, "_id:")) {
            if ((p = strstr(line, "value:")) != NULL) {
                if (strstr(line, "name:adb_enabled ")) g_adb = atoi(p + 6);
                if (strstr(line, "name:adb_wifi_enabled ")) g_adb_wifi = atoi(p + 6);
                if (strstr(line, "name:development_settings_enabled ")) g_dev = atoi(p + 6);
            }
        }

        /* ---- 網路 ---- */
        if (strcmp(service, "wifi") == 0) {
            if (!g_wifi7 && strstr(line, "Wifi7") != NULL) g_wifi7 = 1;
            if (starts_with(line, "time,session,netid,rssi")) {
                /* 有 calculatedTx 欄位的是實測速度，沒有的只有連線速率 */
                g_wifi_score_on = (strstr(line, "calculatedTx") != NULL) ? 1 : 2;
                if (g_wifi_score_on == 2) g_wifi_linkrate = 1;
                continue;
            }
            if (g_wifi_score_on) {
                int rssi, freq, txl, rxl, ctx;
                if (is_log_line(line) || (isdigit((unsigned char)line[0]) && line[2] == '-')) {
                    int got;
                    if (g_wifi_score_on == 1) {
                        got = sscanf(line, "%*[^,],%*d,%*d,%d,%*f,%*f,%d,%d,%d,%*d,%*d,%dMbps", &rssi, &freq, &txl, &rxl, &ctx);
                    } else {
                        got = sscanf(line, "%*[^,],%*d,%*d,%d,%*f,%*f,%d,%d,%d", &rssi, &freq, &txl, &rxl);
                        ctx = txl;
                        if (got == 4) got = 5;
                    }
                    if (got == 5) {
                        if (rssi >= -120 && rssi <= 0) rssi_hist[rssi + 120]++;
                        if (ctx >= 0 && ctx <= 2000) speed_hist[ctx]++;
                        g_wifi_n++;
                        continue;
                    }
                }
                g_wifi_score_on = 0;
            }
            if (is_log_line(line) && g_nwev < MAX_LIST) {
                if (strstr(line, " DATA_STALL_BAD_TX ")) {
                    strncpy(wev[g_nwev].time, line, 11); wev[g_nwev].time[11] = '\0';
                    strcpy(wev[g_nwev].text, "連線卡住（資料送不出去）");
                    g_nwev++;
                } else if (strstr(line, " SCORE_BREACH ") && (p = strstr(line, "lastRssi=")) != NULL && atoi(p + 9) < -75) {
                    strncpy(wev[g_nwev].time, line, 11); wev[g_nwev].time[11] = '\0';
                    sprintf(wev[g_nwev].text, "訊號瞬間掉到 %d dBm", atoi(p + 9));
                    g_nwev++;
                } else if (strstr(line, " CMD_IP_CONFIGURATION_LOST ")) {
                    strncpy(wev[g_nwev].time, line, 11); wev[g_nwev].time[11] = '\0';
                    strcpy(wev[g_nwev].text, "網路位址遺失，Wi-Fi 重新連線");
                    g_nwev++;
                }
            }
        }
        if (strcmp(service, "isub") == 0 && (p = strstr(line, "mSimState[")) != NULL) {
            a = p[10] - '0';
            if (a == 0 || a == 1) copy_until(p + 13, '\n', g_sim[a], sizeof(g_sim[a]));
        }

        /* ---- 背景凍結：記下要觀察的 App 的 uid ---- */
        if (strcmp(service, "package") == 0) {
            if (starts_with(line, "  Package [")) {
                g_pkg_wait = -1;
                for (i = 0; i < watch_n; i++) {
                    sprintf(tmp, "  Package [%s]", watch_pkg[i]);
                    if (starts_with(line, tmp) && watch_uid[i] < 0) g_pkg_wait = i;
                }
            } else if (g_pkg_wait >= 0 && ((p = strstr(line, "appId=")) != NULL || (p = strstr(line, "userId=")) != NULL)) {
                watch_uid[g_pkg_wait] = atoi(strchr(p, '=') + 1);
                g_pkg_wait = -1;
            }
        }

        /* ---- 穩定性 ---- */
        if (g_wait_subject && starts_with(line, "Subject:")) {
            p = strstr(line, "cmp=");
            if (p != NULL) copy_until(p + 4, '/', g_last_anr_name, sizeof(g_last_anr_name));
            else { copy_until(line + 9, '\n', g_last_anr_name, 60); }
            g_wait_subject = 0;
        }
        if (strcmp(section, "ANR FILES") == 0 && (p = strstr(line, " anr_")) != NULL && strstr(line, "temp_anr") == NULL) {
            int y, mo, dd, hh, mi;
            if (sscanf(p + 5, "%d-%d-%d%*c%d-%d", &y, &mo, &dd, &hh, &mi) == 5) {
                char t[16], fname[64];
                sprintf(t, "%02d-%02d %02d:%02d", mo, dd, hh, mi);
                copy_until(p + 1, '\n', fname, sizeof(fname));
                trim(fname);
                if (g_last_anr_file[0] != '\0' && strcmp(fname, g_last_anr_file) == 0 && g_last_anr_name[0] != '\0')
                    add_problem(t, "卡死", g_last_anr_name);
                else
                    add_problem(t, "卡死", "（App 名稱沒有記錄）");
                strcpy(probs[g_nprob - 1].file, fname);
                g_nanr++;
            }
        }
        if (strcmp(service, "dropbox") == 0 && starts_with(line, "20") &&
            (strstr(line, " system_server_crash ") || strstr(line, " system_server_watchdog ") || strstr(line, " SYSTEM_TOMBSTONE "))) {
            char t[16];
            strncpy(t, line + 5, 11); t[11] = '\0';
            copy_until(line + 20, ' ', tmp, sizeof(tmp));
            add_problem(t, "系統崩潰", tmp);
            g_nsys++;
        }

        if (is_log_line(line)) {
            if (strstr(line, " am_kill ") != NULL) handle_kill(line);

            if (g_wait_process && (p = strstr(line, "Process: ")) != NULL) {
                copy_until(p + 9, ',', tmp, sizeof(tmp));
                add_problem(g_crash_time, "閃退", tmp);
                g_ncrash++;
                g_wait_process = 0;
            }
            if (strstr(line, "FATAL EXCEPTION") != NULL) {
                strncpy(g_crash_time, line, 11); g_crash_time[11] = '\0';
                g_wait_process = 1;
            }
            if (strstr(line, "Fatal signal") != NULL) {
                char t[16], name[128] = "";
                char *q = strrchr(line, '(');
                strncpy(t, line, 11); t[11] = '\0';
                if (q != NULL) copy_until(q + 1, ')', name, sizeof(name));
                add_problem(t, "底層崩潰", name);
                g_nnative++;
            }
            if ((p = strstr(line, "ANR in ")) != NULL) {
                char t[16];
                strncpy(t, line, 11); t[11] = '\0';
                copy_until(p + 7, ' ', tmp, sizeof(tmp));
                add_problem(t, "卡死", tmp);
                g_nanr++;
            }

            /* 行動網路（RADIO LOG）那段不看 */
            if (!starts_with(section, "RADIO LOG")) handle_error_line(line);
        }
    }
    g_prog_done = g_prog_total;
    fclose(in);
    printf("讀取完成，共 %ld 行，花了 %.0f 秒\n", total, (double)(clock() - start) / CLOCKS_PER_SEC);
    return total;
}

/* ================= 第 2.5 部分：zip 解壓縮後的其他檔案（共通） ================= */

/* 卡死的 App 名稱沒記錄的話，到 FS\data\anr 裡的檔案找 */
void load_anr_names(void)
{
    char path[1300];
    int k, lines;
    FILE *f;
    char *p;

    for (k = 0; k < g_nprob; k++) {
        if (probs[k].file[0] == '\0' || strstr(probs[k].name, "沒有記錄") == NULL) continue;
        sprintf(path, "%s" SEP "FS" SEP "data" SEP "anr" SEP "%s", g_base, probs[k].file);
        f = fopen(path, "rb");
        if (f == NULL) continue;
        lines = 0;
        while (lines < 200 && fgets(line, LINE_SIZE, f) != NULL) {
            lines++;
            if (starts_with(line, "Subject:")) {
                p = strstr(line, "cmp=");
                if (p != NULL) copy_until(p + 4, '/', probs[k].name, sizeof(probs[k].name));
                else copy_until(line + 9, '\n', probs[k].name, 60);
                break;
            }
        }
        fclose(f);
    }
}

/* 底層崩潰紀錄：FS\data\tombstones\tombstone_00 ～ 31 */
void load_tombstones(void)
{
    char sub[100], name[128], sig[16], when[24], text[128];
    int k, i, lines, signo, y, mo, d, h, mi;
    double t;
    FILE *f;
    char *p;

    for (k = 0; k < 32; k++) {
        sprintf(sub, "data" SEP "tombstones" SEP "tombstone_%02d", k);
        f = open_fs(sub);
        if (f == NULL) continue;
        name[0] = '\0'; when[0] = '\0'; signo = -1; lines = 0;
        while (lines < 40 && fgets(line, LINE_SIZE, f) != NULL) {
            lines++;
            if (starts_with(line, "Timestamp: ")) sprintf(when, "%.16s", line + 11);
            if (starts_with(line, "Cmdline: ")) {
                trim(line);
                p = line + 9;
                /* app_process64 是 App 的外殼，真正的名字在最後面 */
                if (strstr(p, "app_process") != NULL && strrchr(p, ' ') != NULL) p = strrchr(p, ' ') + 1;
                copy_until(p, ' ', name, sizeof(name));
                p = strrchr(name, '/');     /* /vendor/bin/ssgtzd 只留 ssgtzd */
                if (p != NULL && name[0] == '/') memmove(name, p + 1, strlen(p + 1) + 1);
            }
            if (starts_with(line, "signal ")) {
                sscanf(line + 7, "%d (%15[^)]", &signo, sig);
                break;
            }
        }
        fclose(f);
        /* signal 35 是系統自己抓的檢查資料，不是崩潰 */
        if (when[0] == '\0' || signo < 0 || signo == 35) continue;
        sprintf(text, "%.60s (%.15s)", name, sig);
        /* 關機前後 2 分鐘內的崩潰，是關機過程造成的 */
        if (sscanf(when, "%d-%d-%d %d:%d", &y, &mo, &d, &h, &mi) == 5) {
            t = days_of(y, mo, d) * 86400.0 + h * 3600.0 + mi * 60.0;
            for (i = 0; i < g_nsd; i++) {
                if (t - sd_time[i] < 120 && sd_time[i] - t < 120) {
                    strcat(text, "  關機過程中，可忽略");
                    break;
                }
            }
        }
        add_hist(tomb, &g_ntomb, when, text);
    }
    sort_hist(tomb, g_ntomb);
}

void load_history(void)
{
    if (!g_has_dir) return;
    printf("正在讀取歷史紀錄...\n");
    load_anr_names();
    load_tombstones();
    android_history();
    xiaomi_history();        /* 廠商 */
    samsung_history();       /* 廠商 */
    oppo_history();          /* 廠商 */
    vivo_history();          /* 廠商 */
}

/* ================= 第 3 部分：判斷正常 / 注意 ================= */

/* 從長條圖找第 pct% 的值 */
int percentile(int hist[], int n, int total, int pct, int offset)
{
    int i, sum = 0, target = total * pct / 100;
    for (i = 0; i < n; i++) {
        sum += hist[i];
        if (sum > target) return i + offset;
    }
    return n - 1 + offset;
}

/* 算出背景凍結的結果 */
int fz_count[4], fz_packet[4];
double fz_total[4], fz_long[4], gz_span = 0;

void calc_freeze(void)
{
    int w, i;
    double since;
    if (g_ngz == 0) return;
    gz_span = gz[g_ngz - 1].t - gz[0].t;
    for (w = 0; w < watch_n; w++) {
        fz_count[w] = 0; fz_packet[w] = 0; fz_total[w] = 0; fz_long[w] = 0;
        since = -1;
        if (watch_uid[w] < 0) continue;
        for (i = 0; i < g_ngz; i++) {
            if (gz[i].uid != watch_uid[w]) continue;
            if (gz[i].type == 'F') {
                if (since < 0) { since = gz[i].t; fz_count[w]++; }
            } else {
                if (gz[i].type == 'T' && gz[i].packet) fz_packet[w]++;
                if (since >= 0) {
                    double len = gz[i].t - since;
                    fz_total[w] += len;
                    if (len > fz_long[w]) fz_long[w] = len;
                    since = -1;
                }
            }
        }
        if (since >= 0) fz_total[w] += gz[g_ngz - 1].t - since;
    }
}

void judge(void)
{
    int y, m, d, cy, cm, cd;
    memset(cat_warn, 0, sizeof(cat_warn));
    memset(cat_note, 0, sizeof(cat_note));
    if (g_has_dir) g_has_hist = 1;

    /* 系統 */
    if (sscanf(g_patch, "%d-%d-%d", &y, &m, &d) == 3 && sscanf(g_capture, "%d-%d-%d", &cy, &cm, &cd) == 3)
        g_patch_days = days_of(cy, cm, cd) - days_of(y, m, d);
    if (g_adb_wifi == 1) { cat_warn[0] = 1; strcpy(cat_note[0], "無線偵錯開著"); }
    if (g_patch_days > 90) { cat_warn[0] = 1; sprintf(cat_note[0], "安全性更新已經 %d 天沒更新", g_patch_days); }
    if (!g_poweroff_ok) { cat_warn[0] = 1; strcpy(cat_note[0], "上次不是正常關機"); }

    /* 處理器 */
    if (g_time_total > 0 && g_time_min * 100 / g_time_total < 30) { cat_warn[1] = 1; strcpy(cat_note[1], "處理器很少在休息"); }

    /* 記憶體 */
    if (g_kill_bad > 0) { cat_warn[2] = 1; sprintf(cat_note[2], "有 %d 個 App 因異常被關", g_kill_bad); }

    /* 儲存 */
    if (g_life >= 5) { cat_warn[3] = 1; strcpy(cat_note[3], "儲存晶片壽命用掉 4 成以上"); }
    if (g_preeol >= 2) { cat_warn[3] = 1; strcpy(cat_note[3], "儲存晶片發出老化預警"); }
    if (g_ufs_err > 0) { cat_warn[3] = 1; sprintf(cat_note[3], "儲存讀寫錯誤 %ld 次", g_ufs_err); }

    /* 電池 */
    if (g_soh >= 0 && g_soh < 80) { cat_warn[4] = 1; sprintf(cat_note[4], "電池健康度只剩 %d%%", g_soh); }
    if (g_cycle > 800) { cat_warn[4] = 1; sprintf(cat_note[4], "充放電已經 %d 次", g_cycle); }

    /* 網路 */
    if (g_wifi_n > 0) {
        int mr = percentile(rssi_hist, 121, g_wifi_n, 50, -120);
        int ms = percentile(speed_hist, 2001, g_wifi_n, 50, 0);
        if (mr < -75) { cat_warn[5] = 1; sprintf(cat_note[5], "Wi-Fi 訊號太弱（中間值 %d dBm）", mr); }
        if (ms < 50)  { cat_warn[5] = 1; sprintf(cat_note[5], "Wi-Fi 速度太慢（中間值 %d Mbps）", ms); }
    }

    /* 背景凍結：Google Play 服務凍結超過一半時間，又沒有因為訊息被叫醒 */
    calc_freeze();
    if (gz_span > 0 && fz_total[0] / gz_span > 0.5 && fz_packet[0] == 0) {
        cat_warn[6] = 1;
        sprintf(cat_note[6], "Google Play 服務 %d%% 時間被凍結，LINE / Messenger 推播會延遲", (int)(fz_total[0] * 100 / gz_span));
    }

    /* 穩定性 */
    if (g_nprob > 0) { cat_warn[7] = 1; sprintf(cat_note[7], "閃退 %d、卡死 %d、底層崩潰 %d、系統崩潰 %d", g_ncrash, g_nanr, g_nnative, g_nsys); }

    /* 錯誤訊息 */
    if (g_f > 0) { cat_warn[8] = 1; sprintf(cat_note[8], "有 %ld 行致命錯誤", g_f); }

    /* 歷史紀錄：最近 90 天有當機，或有真的開機失敗 */
    if (g_has_hist && sscanf(g_capture, "%d-%d-%d", &cy, &cm, &cd) == 3) {
        int i, recent = 0;
        for (i = 0; i < g_ncrashes; i++) {
            /* 只算系統卡住和自動重開機，子系統重啟不算當機 */
            if (strstr(crashes[i].text, "重新啟動") != NULL) continue;
            if (sscanf(crashes[i].time, "%d-%d-%d", &y, &m, &d) == 3 && days_of(cy, cm, cd) - days_of(y, m, d) <= 90) recent++;
        }
        if (recent > 0) { cat_warn[9] = 1; sprintf(cat_note[9], "最近 90 天有 %d 次當機紀錄", recent); }
        if (g_real_bootfail > 0) { cat_warn[9] = 1; sprintf(cat_note[9], "有 %d 次開機失敗", g_real_bootfail); }
    }

    /* 溫度：電池或機身超過 45 度、處理器超過 90 度、系統發熱等級 2 以上 */
    if (xt[2] > 45 || xt[3] > 45) { cat_warn[10] = 1; sprintf(cat_note[10], "電池 / 機身溫度偏高 (%.0f°C)", xt[2] > xt[3] ? xt[2] : xt[3]); }
    if (xt[0] > 90) { cat_warn[10] = 1; sprintf(cat_note[10], "處理器溫度 %.0f°C", xt[0]); }
    if (xt_status >= 2) { cat_warn[10] = 1; sprintf(cat_note[10], "系統發熱等級 %d，效能會被降低", xt_status); }

    /* 安全檢查：有開無障礙服務的 App、沒有螢幕鎖、無線偵錯開著、Bootloader 解鎖 */
    if (sc_a11y[0] && strcmp(sc_a11y, "-") != 0) { cat_warn[11] = 1; strcpy(cat_note[11], "有 App 開了無障礙服務，請確認是你認得的"); }
    if (strcmp(sc_lock, "NONE") == 0) { cat_warn[11] = 1; strcpy(cat_note[11], "沒有設定螢幕鎖"); }
    if (g_locked[0] && strcmp(g_locked, "1") != 0) { cat_warn[11] = 1; strcpy(cat_note[11], "Bootloader 沒有上鎖"); }
}

/* ================= 第 4 部分：各分頁 ================= */

const char *status(int cat) { return cat_warn[cat] ? "[注意]" : "[正常]"; }
const char *or_none(const char *s) { return s[0] ? s : "未記錄"; }

/* 儲存晶片廠商：先查代碼，查不到用報告裡寫的名字 */
const char *ufs_maker(void)
{
    const char *n = lookup(g_ufs_code, ufs_code, ufs_name, ufs_n);
    if (n != NULL) return n;
    if (g_ufs_maker[0]) return g_ufs_maker;
    return NULL;
}

/* 儲存容量（GB） */
int ufs_size(void)
{
    if (g_ufs_gb > 0) return g_ufs_gb;
    if (g_data_total_kb > 0) return round_size(g_data_total_kb / 1024.0 / 1024.0, rom_sizes, 8);
    return 0;
}

void page_overview(void)
{
    int i, ok = 0, warn = 0;
    for (i = 0; i < CAT_N; i++) {
        if (i == 9 && !g_has_hist) continue;     /* 沒有歷史紀錄就不算 */
        if (cat_warn[i]) warn++; else ok++;
    }
    printf("\n============ 總覽 ============\n");
    printf(" 正常 %d 項   注意 %d 項\n\n", ok, warn);
    for (i = 0; i < CAT_N; i++) {
        if (cat_warn[i]) printf(" [注意] %-12s %s\n", cat_name[i], cat_note[i]);
    }
    if (warn == 0) printf(" 全部正常\n");
}

void page_system(void)
{
    printf("\n============ 系統 ============\n");
    printf(" 型號         %s (%s)\n", or_none(g_model), or_none(g_model_code));
    printf(" Android      %s\n", or_none(g_android));
    if (g_os_name[0]) printf(" %-12s %s\n", g_os_name, or_none(g_os_ver));
    if (g_fw[0]) printf(" 韌體版本     %s\n", g_fw);
    if (g_patch_days >= 0) printf(" 安全性更新   %s (%d 天前)\n", g_patch, g_patch_days);
    else                   printf(" 安全性更新   %s\n", or_none(g_patch));
    printf(" Bootloader   %s，驗證狀態 %s\n", strcmp(g_locked, "1") == 0 ? "已上鎖" : "未上鎖", or_none(g_verified));
    note("安全性更新超過 90 天沒更新，比較容易被已知漏洞攻擊");
    note("Bootloader 上鎖、驗證狀態 green，代表系統是原廠的、沒有被改過");
    if (g_warranty[0]) printf(" Knox 保固    %s\n", strcmp(g_warranty, "0") == 0 ? "正常（沒有刷過機）" : "已觸發（刷過非官方系統）");
    printf(" 抓取時間     %s\n", or_none(g_capture));
    printf(" 已開機       %s\n", or_none(g_uptime));
    printf(" 上次關機     %s%s\n", or_none(g_poweroff), g_poweroff_ok ? "" : "  [注意]");
    printf(" 開發者選項   %s\n", g_dev == 1 ? "開啟" : "關閉");
    printf(" USB 偵錯     %s\n", g_adb == 1 ? "開啟" : "關閉");
    printf(" 無線偵錯     %s%s\n", g_adb_wifi == 1 ? "開啟" : "關閉", g_adb_wifi == 1 ? "  [注意] 不用時建議關掉" : "");
    ex_system_more();
}

void page_cpu(void)
{
    printf("\n======== 處理器與螢幕 ========\n");
    printf(" 處理器       %s %s %s (%d 核)\n", g_soc_maker, or_none(g_soc), soc_name(g_soc), g_cores);
    if (g_freq_max > 0) {
        printf(" 頻率範圍     %ld MHz ～ %.1f GHz (第一組核心)\n", g_freq_min / 1000, g_freq_max / 1000000.0);
        printf(" 休息時間     %ld%% 的時間在低頻率 %s\n", g_time_min * 100 / g_time_total, status(1));
        note("處理器大部分時間待在低頻率，代表平常沒有東西一直在背景耗電；低於 30% 會標示注意");
    }
    printf(" 螢幕         %s %d x %d\n", g_oled ? "OLED" : "", g_w, g_h);
    printf(" 更新率       最高 %.0f Hz", g_max_hz);
    if (g_hz_setting[0]) printf("，目前設定 %s", g_hz_setting);
    printf("\n");
    ex_cpu_more();
}

void page_parts(void)
{
    const char *n;
    int i;
    printf("\n========= 零件與規格 =========\n");
    printf(" 處理器     %s %s %s\n", g_soc_maker, or_none(g_soc), soc_name(g_soc));
    printf(" 圖形晶片   %s\n", or_none(g_gpu));

    n = lookup(g_dram_code, dram_code, dram_name, dram_n);
    if (n != NULL) printf(" 記憶體     %s  %s  %d GB\n", n, g_dram_type, g_dram_gb);
    else if (g_dram_code < 0) printf(" 記憶體     %d GB (報告裡沒有記錄廠商)\n", round_size(g_memtotal_kb / 1024.0 / 1024.0, ram_sizes, 9));
    else           printf(" 記憶體     未知 (代碼 %d)  %s  %d GB\n", g_dram_code, g_dram_type, g_dram_gb);
    if (g_dram_ch > 0) printf("            %d 通道, 最高 %d MT/s\n", g_dram_ch, g_dram_freq);

    n = ufs_maker();
    printf(" 儲存       %s  %s\n", n != NULL ? n : "未知", g_ufs_model);
    if (g_ufs_ver > 0) printf("            %d GB  UFS %d.%d (%s)\n", ufs_size(), g_ufs_ver >> 8, (g_ufs_ver >> 4) & 0xF, g_ufs_gear);
    else if (ufs_size() > 0) printf("            %d GB\n", ufs_size());

    printf(" 螢幕       %s\n", or_none(g_panel_text));

    if (strstr(g_touch, "goodix")) printf(" 觸控       匯頂 Goodix\n");
    else if (starts_with(g_touch, "stm")) printf(" 觸控       意法半導體 STMicroelectronics\n");
    else if (starts_with(g_touch, "slsi")) printf(" 觸控       三星 Samsung LSI\n");
    else if (strstr(g_touch, "synaptics")) printf(" 觸控       Synaptics\n");
    else if (strstr(g_touch, "fts") || strstr(g_touch, "focal")) printf(" 觸控       敦泰 FocalTech\n");
    else if (strstr(g_touch, "nvt")) printf(" 觸控       聯詠 Novatek\n");
    else printf(" 觸控       %s\n", or_none(g_touch));

    printf(" 指紋       %s\n", or_none(g_fp_text));

    for (i = 0; i < g_ncam; i++) {
        printf(" %s %s %s\n", i == 0 ? "鏡頭      " : "          ", camera_maker(g_cams[i]), g_cams[i]);
    }

    for (i = 0; i < g_ncharger; i++) {
        const char *maker = "";
        if (starts_with(g_chargers[i], "mt")) maker = "聯發科";
        else if (starts_with(g_chargers[i], "sc")) maker = "南芯";
        else if (starts_with(g_chargers[i], "bq")) maker = "德州儀器";
        else if (starts_with(g_chargers[i], "nu")) maker = "伏達";
        else if (starts_with(g_chargers[i], "max")) maker = "Maxim (ADI)";
        else if (starts_with(g_chargers[i], "pca")) maker = "NXP";
        else if (starts_with(g_chargers[i], "p9")) maker = "Renesas (IDT) 無線充電";
        else if (starts_with(g_chargers[i], "s2m")) maker = "三星 Samsung LSI";
        printf(" %s %s %s\n", i == 0 ? "充電      " : "          ", maker, g_chargers[i]);
    }

    ex_amp_line();
    if (g_wifi_chip[0]) printf(" Wi-Fi/藍牙 聯發科 %s%s\n", g_wifi_chip, g_wifi7 ? " (Wi-Fi 7)" : "");
    note("LPDDR 是記憶體規格、UFS 是儲存規格，數字越大越新越快；HS-G 是儲存傳輸的速度檔位");
    note("鏡頭顯示的是感光元件型號，報告只會記錄用過的鏡頭，可能不完整");
}

void page_memory(void)
{
    int i;
    printf("\n=========== 記憶體 ===========\n");
    if (g_dram_gb > 0) printf(" 總量   %d GB\n", g_dram_gb);
    else               printf(" 總量   %d GB (系統可用 %.1f GB)\n", round_size(g_memtotal_kb / 1024.0 / 1024.0, ram_sizes, 9), g_memtotal_kb / 1024.0 / 1024.0);
    ex_memory_more();
    printf("\n ---- 特殊原因被關的 App ----\n");
    if (g_nkill == 0) printf(" 沒有\n");
    for (i = g_nkill - 1; i >= 0; i--) {
        printf(" %s  %s\n", kills[i].time, kills[i].proc);
        printf("              %s\n", kills[i].reason);
    }
    if (g_nkill > 0) printf(" 共 %d 筆\n", g_nkill);
    if (g_nunknown > 0) printf(" 尚未收錄的原因已寫進 important.txt\n");
}

void page_storage(void)
{
    const char *n = ufs_maker();
    printf("\n========== 儲存空間 ==========\n");
    printf(" 容量       %d GB (%s)\n", ufs_size(), n != NULL ? n : "未知廠商");
    if (g_life == 0x0B)     printf(" 壽命       已超過設計壽命  [注意]\n");
    else if (g_life >= 1)   printf(" 壽命       已用 %d ～ %d%%  %s\n", (g_life - 1) * 10, g_life * 10, g_life >= 5 ? "[注意]" : "[正常]");
    else                    printf(" 壽命       未記錄\n");
    if (g_preeol >= 0)      printf(" 老化預警   %s\n", g_preeol <= 1 ? "正常" : (g_preeol == 2 ? "警告  [注意]" : "緊急  [注意]"));
    printf(" 讀寫錯誤   %ld 次  %s\n", g_ufs_err, g_ufs_err > 0 ? "[注意]" : "[正常]");
    note("壽命是儲存晶片自己回報的磨損程度，以 10% 為一級；讀寫錯誤不是 0 的話建議先備份資料");
    ex_storage_more();
}

void page_battery(void)
{
    printf("\n============ 電池 ============\n");
    if (g_soh >= 0)   printf(" 健康度        %d%%      %s\n", g_soh, g_soh < 80 ? "[注意]" : "[正常]");
    else              printf(" 健康度        未記錄\n");
    if (g_asoc >= 0)  printf(" 剩餘容量      %d%% (和新電池比)\n", g_asoc);
    if (g_cycle >= 0) printf(" 充放電次數    %d 次   %s\n", g_cycle, g_cycle > 800 ? "[注意]" : "[正常]");
    if (g_cycle_note[0]) printf("               %s\n", g_cycle_note);
    if (strlen(g_first_use) == 8) printf(" 開始使用      %.4s-%.2s-%.2s\n", g_first_use, g_first_use + 4, g_first_use + 6);
    note("健康度是電池現在能充滿的容量和全新時的比例，低於 80% 建議換電池；用掉 100% 的電算一次充放電");
    ex_battery_now();
    if (g_cv <= 0 && g_sd_min <= 0) return;     /* 沒有電壓資料，就不顯示 */
    printf("\n ---- 這顆電池的電壓範圍 ----\n");
    if (g_cv > 0) printf(" 充電上限   %.2f V   (充到這個電壓就停)\n", g_cv > 100000 ? g_cv / 1000000.0 : g_cv / 1000.0);
    else          printf(" 充電上限   未記錄\n");
    if (g_sd_min > 0) printf(" 關機電壓   %.2f ～ %.2f V (低於這個系統強制關機)\n", g_sd_min / 1000.0, g_sd_max / 1000.0);
    else              printf(" 關機電壓   未記錄\n");
}

void page_network(void)
{
    int i;
    printf("\n============ 網路 ============\n");
    if (g_wifi_n == 0) {
        printf(" Wi-Fi   沒有記錄\n");
    } else {
        int rmin = percentile(rssi_hist, 121, g_wifi_n, 0, -120);
        int rmax = -120;
        int r10 = percentile(rssi_hist, 121, g_wifi_n, 10, -120);
        int r90 = percentile(rssi_hist, 121, g_wifi_n, 90, -120);
        int s10 = percentile(speed_hist, 2001, g_wifi_n, 10, 0);
        int s90 = percentile(speed_hist, 2001, g_wifi_n, 90, 0);
        int smin = percentile(speed_hist, 2001, g_wifi_n, 0, 0);
        for (i = 0; i < 121; i++) if (rssi_hist[i] > 0) rmax = i - 120;   /* 最強的訊號 */
        printf(" Wi-Fi   %s\n", cat_warn[5] ? "[注意]" : "正常");
        printf("   訊號   %d ～ %d dBm，大多在 %d ～ %d dBm\n", rmax, rmin, r90, r10);
        if (g_wifi_linkrate) printf("   連線速率 大多 %d ～ %d Mbps，最低瞬間 %d Mbps (理論值，不是實測速度)\n", s10, s90, smin);
        else                 printf("   速度   大多 %d ～ %d Mbps，最低瞬間 %d Mbps\n", s10, s90, smin);
        if (cat_warn[5]) printf("   %s\n", cat_note[5]);
        if (g_nwev > 0) {
            printf("   短暫狀況 %d 次:\n", g_nwev);
            for (i = g_nwev - 1; i >= 0; i--) printf("     %s  %s\n", wev[i].time, wev[i].text);
        }
    }
    if (g_wifi_n > 0) note("訊號 dBm 越接近 0 越強：-50 以上很好、-70 普通、-80 以下很弱；Mbps 是每秒傳多少百萬位元");
    if (strcmp(g_sim[0], "ABSENT") == 0 && strcmp(g_sim[1], "ABSENT") == 0) printf(" SIM     未插卡\n");
    else if (g_sim[0][0] || g_sim[1][0]) printf(" SIM     卡槽 1: %s  卡槽 2: %s\n", or_none(g_sim[0]), or_none(g_sim[1]));
    else printf(" SIM     未記錄\n");
    ex_network_more();
}

void page_freeze(void)
{
    int w;
    char a[32], b[32];
    printf("\n========== 背景凍結 ==========\n");
    if (g_ngz == 0) { printf(" 沒有凍結紀錄\n"); return; }
    sec_to_text(gz_span, a);
    printf(" 紀錄期間約 %s\n", a);
    if (g_freeze_note[0]) printf(" %s\n", g_freeze_note);
    printf("\n");
    for (w = 0; w < watch_n; w++) {
        if (watch_uid[w] < 0) { printf(" %-16s 沒有安裝\n\n", watch_cn[w]); continue; }
        if (fz_count[w] == 0) { printf(" %-16s 這段期間沒有被凍結\n\n", watch_cn[w]); continue; }
        sec_to_text(fz_total[w], a);
        sec_to_text(fz_long[w], b);
        printf(" %s%s\n", watch_cn[w], w == 0 ? " (LINE/Messenger 推播用)" : "");
        printf("   凍結 %d 次，共 %s (佔 %d%%)%s\n", fz_count[w], a, (int)(fz_total[w] * 100 / gz_span),
               (w == 0 && cat_warn[6]) ? "  [注意]" : "");
        printf("   最長一次 %s\n", b);
        printf("   有訊息時被叫醒 %d 次\n\n", fz_packet[w]);
    }
    if (cat_warn[6]) printf(" 建議: 把 Google Play 服務、LINE、Messenger 的省電策略改成「無限制」\n");
    note("背景凍結是系統把沒在用的 App 暫停來省電，凍結太久會讓推播延遲");
}

void page_stability(void)
{
    int i;
    printf("\n=========== 穩定性 ===========\n");
    printf(" App 閃退          %d 次   %s\n", g_ncrash, g_ncrash ? "[注意]" : "[正常]");
    printf(" 底層崩潰          %d 次   %s\n", g_nnative, g_nnative ? "[注意]" : "[正常]");
    printf(" 系統崩潰          %d 次   %s\n", g_nsys, g_nsys ? "[注意]" : "[正常]");
    printf(" App 卡死 (ANR)    %d 次   %s\n", g_nanr, g_nanr ? "[注意]" : "[正常]");
    note("閃退是 App 突然關掉；卡死 (ANR) 是 App 超過 5 秒沒反應；底層崩潰是用 C/C++ 寫的程式崩潰；系統崩潰是整個系統出錯");
    if (g_nprob > 0) {
        printf("\n ---- 詳細 ----\n");
        for (i = 0; i < g_nprob; i++) printf(" %s  %-8s %s\n", probs[i].time, probs[i].kind, probs[i].name);
    }
}

void page_history(void)
{
    int i;
    printf("\n========== 歷史紀錄 ==========\n");
    if (!g_has_hist) {
        printf(" 這一頁要選 bugreport 的 zip 檔才有資料\n");
        return;
    }

    printf(" ---- 系統更新 ----\n");
    if (g_nupd == 0) printf(" 沒有紀錄\n");
    for (i = 0; i < g_nupd; i++) printf(" %s  %s  已安裝\n", upd[i].time, upd[i].text);

    printf("\n ---- 恢復原廠 / recovery ----\n");
    if (g_nrst == 0) printf(" 沒有紀錄\n");
    for (i = 0; i < g_nrst; i++) printf(" %-16s  %s\n", rst[i].time, rst[i].text);

    printf("\n ---- 過去的當機紀錄 ----\n");
    if (g_ncrashes == 0) printf(" 沒有紀錄\n");
    for (i = 0; i < g_ncrashes; i++) printf(" %s  %s\n", crashes[i].time, crashes[i].text);

    if (g_has_dir) {
        printf("\n ---- 程式底層崩潰 ----\n");
        if (g_ntomb == 0) printf(" 沒有紀錄\n");
        for (i = 0; i < g_ntomb; i++) printf(" %s  %s\n", tomb[i].time, tomb[i].text);
    }

    android_history_page();
    xiaomi_history_page();       /* 廠商 */
    samsung_history_page();      /* 廠商 */
    oppo_history_page();         /* 廠商 */
    vivo_history_page();         /* 廠商 */
}

void page_errors(void)
{
    printf("\n========== 錯誤訊息 ==========\n");
    printf(" E 錯誤      %ld 行\n", g_e);
    printf(" F 致命      %ld 行  %s\n", g_f, g_f ? "[注意]" : "");
    printf(" 黑名單排除  %ld 行 (%d 種系統雜訊)\n", g_black, blacklist_n);
    printf(" 剩下要看的  %ld 行\n\n", g_kept);
    note("E 是錯誤、F 是嚴重錯誤。大部分是 App 或系統的雜訊，不一定代表手機有問題");
    printf(" 詳細內容已存到 important.txt\n");
}

/* ================= 第 5 部分：更多資料的分頁 ================= */

/* 系統頁加上的項目 */
void ex_system_more(void)
{
    const char *av[] = {"10", "11", "12", "12L", "13", "14", "15", "16", "17", "18"};
    printf("\n ---- 系統細節 ----\n");
    if (x_first_api >= 29 && x_first_api <= 38) printf(" 出廠時的系統 Android %s\n", av[x_first_api - 29]);
    if (x_build_date[0]) printf(" 系統編譯日期 %s\n", x_build_date);
    if (x_kernel[0])     printf(" 核心版本     Linux %s\n", x_kernel);
    if (x_baseband[0])   printf(" 基頻版本     %s\n", x_baseband);
    if (x_kernel[0] || x_baseband[0]) note("核心是系統最底層、管理硬體的程式；基頻是控制行動網路晶片的韌體");
    if (x_gms[0])        printf(" Google 服務  %s\n", x_gms);
    if (x_region[0]) {
        const char *r = x_region;
        if (strcmp(x_region, "CN") == 0) r = "中國版 (CN)";
        else if (strcmp(x_region, "GLOBAL") == 0 || strcmp(x_region, "GL") == 0) r = "國際版 (GLOBAL)";
        else if (strcmp(x_region, "TW") == 0) r = "台灣版 (TW)";
        printf(" 銷售地區     %s\n", r);
    }
    if (x_locale[0]) printf(" 語言         %s\n", x_locale);
    if (x_tz[0])     printf(" 時區         %s\n", x_tz);
    if (x_crypto[0]) printf(" 資料加密     %s\n", strcmp(x_crypto, "encrypted") == 0 ? "已加密" : x_crypto);
    if (x_slot[0])   printf(" 系統分割區   A/B 雙分割區，目前使用 %c 槽\n", toupper((unsigned char)x_slot[strlen(x_slot) - 1]));
}

/* 處理器與螢幕頁加上的項目 */
void ex_cpu_more(void)
{
    int i, j;
    double t;
    if (xdp_nrates > 0) {
        for (i = 0; i < xdp_nrates - 1; i++)
            for (j = 0; j < xdp_nrates - 1 - i; j++)
                if (xdp_rates[j] < xdp_rates[j+1]) { t = xdp_rates[j]; xdp_rates[j] = xdp_rates[j+1]; xdp_rates[j+1] = t; }
        printf(" 可用更新率  ");
        for (i = 0; i < xdp_nrates; i++) printf(" %.0f", xdp_rates[i] + 0.0001);
        printf(" Hz\n");
    }
    if (xdp_density > 0) printf(" 像素密度     %.0f ppi (系統顯示密度 %d dpi)\n", xdp_dpi, xdp_density);
    if (xdp_hdr[0])      printf(" HDR 格式     %s\n", xdp_hdr);
    if (xdp_density > 0) note("ppi 是每英吋有幾個像素，越高越細緻；dpi 是系統排版用的密度，和實際像素不同");
    if (xdp_hdr[0]) note("HDR 讓影片的亮部、暗部細節更多，要影片本身也支援才看得到差別");
}

/* 記憶體頁加上的項目 */
void ex_memory_more(void)
{
    if (xm_avail >= 0) printf(" 目前可用 %.1f GB\n", xm_avail / 1024.0 / 1024.0);
    if (xm_swap_total > 0 && xm_swap_free >= 0) {
        long used = xm_swap_total - xm_swap_free;
        printf(" 虛擬記憶體 已用 %.1f / %.1f GB (%ld%%)%s\n", used / 1024.0 / 1024.0, xm_swap_total / 1024.0 / 1024.0,
               used * 100 / xm_swap_total, used * 100 / xm_swap_total >= 95 ? "  快滿了，背景 App 容易被關" : "");
        note("虛擬記憶體是把不常用的資料壓縮起來放，記憶體不夠時用的，快滿代表記憶體很吃緊");
    }
}

/* 儲存空間頁加上的項目 */
void ex_storage_more(void)
{
    char a[32];
    int i, j, k, idx[10], n = 0;
    if (xd_app < 0) return;
    printf("\n ---- 空間用在哪裡 ----\n");
    if (xd_free_kb >= 0 && g_data_total_kb > 0) {
        bytes_text(xd_free_kb * 1024.0, a);
        printf(" 剩餘空間     %s (%ld%%)\n", a, xd_free_kb * 100 / g_data_total_kb);
    }
    bytes_text(xd_app, a);   printf(" App 本體     %s\n", a);
    if (xd_data >= 0)  { bytes_text(xd_data, a);  printf(" App 資料     %s\n", a); }
    if (xd_cache >= 0) { bytes_text(xd_cache, a); printf(" App 快取     %s (可以清除)\n", a); }
    if (xd_photo >= 0) { bytes_text(xd_photo, a); printf(" 照片         %s\n", a); }
    if (xd_video >= 0) { bytes_text(xd_video, a); printf(" 影片         %s\n", a); }
    if (xd_audio >= 0) { bytes_text(xd_audio, a); printf(" 音樂與錄音   %s\n", a); }
    if (xd_other >= 0) { bytes_text(xd_other, a); printf(" 其他檔案     %s\n", a); }
    if (xd_write >= 0) printf(" 最近寫入速度 %.0f MB/s\n", xd_write / 1024.0);

    /* 佔最多空間的 App（本體 + 資料） */
    if (dn_n == 0) return;
    for (k = 0; k < 10; k++) {
        int best = -1;
        for (i = 0; i < dn_n; i++) {
            for (j = 0; j < n; j++) if (idx[j] == i) break;
            if (j < n) continue;
            if (best < 0 || dn_size[i] > dn_size[best]) best = i;
        }
        if (best < 0 || dn_size[best] <= 0) break;
        idx[n++] = best;
    }
    printf("\n ---- 最佔空間的 App ----\n");
    for (i = 0; i < n; i++) {
        char lab[160];
        bytes_text(dn_size[idx[i]], a);
        app_label(dn_name[idx[i]], lab);
        printf(" %2d. %-10s %s\n", i + 1, a, lab);
    }
}

/* 電池頁加上的「目前狀態」 */
void ex_battery_now(void)
{
    if (xb_level < 0) return;
    printf("\n ---- 抓取當下的狀態 ----\n");
    printf(" 電量       %d%%\n", xb_level);
    if (xb_status == 2) printf(" 狀態       充電中%s\n", xb_ac ? " (插座)" : xb_usb ? " (USB)" : xb_wl ? " (無線)" : "");
    else if (xb_status == 5) printf(" 狀態       已充飽\n");
    else printf(" 狀態       沒有充電\n");
    if (xb_volt > 1000) printf(" 電壓       %.2f V\n", xb_volt / 1000.0);
    if (xb_temp != -9999) printf(" 溫度       %.1f°C%s\n", xb_temp / 10.0, xb_temp > 450 ? "  [注意] 偏熱" : "");
    if (xb_tech[0]) printf(" 電池種類   %s\n", strcmp(xb_tech, "Li-poly") == 0 ? "鋰聚合物 (Li-poly)" : strcmp(xb_tech, "Li-ion") == 0 ? "鋰離子 (Li-ion)" : xb_tech);
    if (xb_health >= 0 && xb_health != 2) printf(" 電池狀況   系統回報異常 (代碼 %d)  [注意]\n", xb_health);
    if (xb_counter > 0 && xb_level >= 20) {
        printf(" 目前電量約 %ld mAh，換算充滿約 %ld mAh\n", xb_counter / 1000, xb_counter / 10 / xb_level);
        printf("            (由電量計推算，有些手機不準，健康度以上面為準)\n");
    }
    if (xp_capacity > 0) printf(" 系統記錄的電池容量 %ld mAh\n", xp_capacity);
}

/* 網路頁加上的項目 */
void ex_network_more(void)
{
    char op[96], nt[48], *p;
    if (xw_link >= 0 && xw_freq > 0) {
        const char *band = xw_freq >= 5925 ? "6 GHz" : xw_freq >= 4900 ? "5 GHz" : "2.4 GHz";
        const char *gen = "";
        if (strcmp(xw_std, "11be") == 0) gen = "Wi-Fi 7";
        else if (strcmp(xw_std, "11ax") == 0) gen = "Wi-Fi 6";
        else if (strcmp(xw_std, "11ac") == 0) gen = "Wi-Fi 5";
        else if (strcmp(xw_std, "11n") == 0) gen = "Wi-Fi 4";
        printf("\n ---- 抓取當下的 Wi-Fi ----\n");
        printf(" 規格   %s %s，%s\n", gen, xw_std, band);
        if (xw_rssi != 999) printf(" 訊號   %d dBm\n", xw_rssi);
        printf(" 連線速率 %d Mbps\n", xw_link);
    }
    if (x_sim_op[0] && strcmp(x_sim_op, ",") != 0) {
        strcpy(op, x_sim_op);
        strcpy(nt, x_net_type);
        printf("\n ---- 行動網路 ----\n");
        p = strchr(op, ',');
        if (p != NULL) *p = '\0';
        if (op[0]) {
            char t1[24] = "";
            copy_until(nt, ',', t1, sizeof(t1));
            printf(" 卡槽 1  %s  %s\n", op, strcmp(t1, "Unknown") == 0 ? "" : t1);
        }
        if (p != NULL && p[1]) {
            char *n2 = strchr(nt, ',');
            printf(" 卡槽 2  %s  %s\n", p + 1, (n2 != NULL && strcmp(n2 + 1, "Unknown") != 0) ? n2 + 1 : "");
        }
    }
}

/* 零件頁的喇叭放大器 */
void ex_amp_line(void)
{
    if (xa_chip[0]) {
        const char *maker = "";
        int n = xa_naddr > xa_aw ? xa_naddr : xa_aw;
        if (starts_with(xa_chip, "CS")) maker = "Cirrus Logic";
        else if (starts_with(xa_chip, "AW")) maker = "艾為 Awinic";
        if (n >= 2) printf(" 喇叭放大器 %s%s%s x %d\n", maker, maker[0] ? " " : "", xa_chip, n);
        else        printf(" 喇叭放大器 %s%s%s\n", maker, maker[0] ? " " : "", xa_chip);
    } else if (g_amp[0]) {
        const char *maker = "";
        if (starts_with(g_amp, "cs")) maker = "Cirrus Logic";
        else if (starts_with(g_amp, "aw")) maker = "艾為 Awinic";
        else if (starts_with(g_amp, "tfa")) maker = "NXP";
        printf(" 喇叭放大器 %s %s\n", maker, g_amp);
    }
    if (xa_haptic[0]) printf(" 震動馬達   %s\n", xa_haptic);
}

/* 溫度頁 */
void page_thermal(void)
{
    const char *tn[] = {"處理器    ", "圖形晶片  ", "電池      ", "機身表面  ", "USB 接口  ", "訊號放大器"};
    const char *st[] = {"正常", "輕微發熱", "中度發熱，可能降速", "嚴重發熱，會降速", "非常燙，大幅降速", "緊急", "即將關機"};
    int i, any = 0;
    printf("\n============ 溫度 ============\n");
    printf(" (抓取報告當下的溫度)\n");
    for (i = 0; i < 6; i++) {
        if (xt[i] <= 0) continue;
        any = 1;
        printf(" %s %.1f°C%s\n", tn[i], xt[i],
               ((i == 2 && xt[i] > 45) || (i == 3 && xt[i] > 45) || (i <= 1 && xt[i] > 90)) ? "  [注意]" : "");
    }
    if (!any && xb_temp != -9999) { printf(" 電池       %.1f°C\n", xb_temp / 10.0); any = 1; }
    if (!any) printf(" 沒有記錄\n");
    if (xt_status >= 0 && xt_status <= 6) printf("\n 發熱等級   %d (%s)%s\n", xt_status, st[xt_status], xt_status >= 2 ? "  [注意]" : "");
    note("發熱等級 0 是正常，2 以上系統會降低效能來降溫；訊號放大器是行動網路的發射元件");
}

/* 感測器頁 */
void page_sensors(void)
{
    int i;
    printf("\n=========== 感測器 ===========\n");
    if (xs_total == 0) { printf(" 沒有記錄\n"); return; }
    printf(" 系統一共回報 %d 個感測器 (含軟體算出來的)\n", xs_total);
    note("下面只列實體感測器，括號裡是晶片廠商");
    printf("\n");
    for (i = 0; i < xs_n; i++) {
        printf(" %s\n", sensor_cn(xs[i].type));
        printf("   %s  (%s)\n", xs[i].name, xs[i].vendor);
    }
}

/* 音訊與震動頁 */
void page_audio(void)
{
    int n = xa_naddr > xa_aw ? xa_naddr : xa_aw;
    printf("\n========= 音訊與震動 =========\n");
    printf(" 喇叭       %s\n", xa_stereo ? "立體聲 (2 個喇叭)" : n == 1 ? "單聲道" : "未記錄");
    ex_amp_line();
    if (xa_chip[0] == '\0' && g_amp[0] == '\0' && xa_haptic[0] == '\0') printf(" 報告裡沒有記錄喇叭和震動的晶片\n");
    printf("\n 報告裡沒有喇叭本身的尺寸、瓦數等規格，只有控制晶片\n");
    note("喇叭放大器負責把聲音放大推動喇叭；震動馬達晶片控制觸感回饋，好的晶片震感比較清脆");
}

/* 藍牙頁 */
void page_bluetooth(void)
{
    int i, n;
    printf("\n============ 藍牙 ============\n");
    if (xbt_state[0] == '\0' && xbt_bonded < 0) { printf(" 沒有記錄\n"); return; }
    if (xbt_state[0]) printf(" 狀態       %s\n", strcmp(xbt_state, "ON") == 0 ? "開啟" : strcmp(xbt_state, "OFF") == 0 ? "關閉" : strcmp(xbt_state, "BLE_ON") == 0 ? "只開省電藍牙 (BLE)" : xbt_state);
    if (xbt_name[0])  printf(" 裝置名稱   %s\n", xbt_name);
    n = xbt_bonded > xbt_ndev ? xbt_bonded : xbt_ndev;
    if (n >= 0) printf(" 配對過的裝置 %d 個\n", n);
    for (i = 0; i < xbt_ndev; i++) printf("   %s\n", xbt_dev[i]);
    if (n > 0 && xbt_ndev == 0) printf("   (這個系統的報告沒有寫裝置名稱)\n");
}

/* 從 n 筆裡找最大的前 k 筆（用 key 函式比較），結果放進 idx，回傳個數 */
int top_k(int n, int k, double (*key)(int), int *idx)
{
    int i, j, c = 0;
    while (c < k) {
        int best = -1;
        for (i = 0; i < n; i++) {
            for (j = 0; j < c; j++) if (idx[j] == i) break;
            if (j < c) continue;
            if (best < 0 || key(i) > key(best)) best = i;
        }
        if (best < 0 || key(best) <= 0) break;
        idx[c++] = best;
    }
    return c;
}
double key_us(int i) { return (double)us[i].sec; }
double key_nt(int i) { return nt[i].rx + nt[i].tx; }
double key_al(int i) { return (double)al[i].wakes; }
double key_dh(int i) { return (double)dh[i].drop; }

/* 字串日期比大小用：把 "2026-11-18 19:27:16" 變成數字 */
double date_key(const char *s)
{
    int y, mo, d, h = 0, mi = 0;
    if (sscanf(s, "%d-%d-%d %d:%d", &y, &mo, &d, &h, &mi) < 3 || y < 2000) return 0;
    return days_of(y, mo, d) * 1440.0 + h * 60 + mi;
}
double key_first(int i) { return pk_sys[i] ? 0 : date_key(pk_first[i]); }
double key_last(int i)  { return pk_sys[i] ? 0 : date_key(pk_last[i]); }

/* 應用程式頁 */
void page_apps(void)
{
    char a[48], lab[160];
    int i, n, idx[15], user = 0, sys = 0;
    printf("\n========== 應用程式 ==========\n");
    for (i = 0; i < pk_n; i++) { if (pk_sys[i]) sys++; else user++; }
    if (pk_n > 0) printf(" 已安裝 %d 個 (自己裝的 %d 個，系統內建 %d 個)\n", pk_n, user, sys);
    else if (dn_n > 0) printf(" 已安裝 %d 個套件 (含系統內建)\n", dn_n);

    n = top_k(pk_n, 10, key_first, idx);
    if (n > 0) {
        printf("\n ---- 最近安裝的 App ----\n");
        for (i = 0; i < n; i++) {
            app_label(pk_name[idx[i]], lab);
            printf(" %.10s  %s%s%s\n", pk_first[idx[i]], lab, pk_ver[idx[i]][0] ? "  v" : "", pk_ver[idx[i]]);
        }
    }
    n = top_k(pk_n, 10, key_last, idx);
    if (n > 0) {
        printf("\n ---- 最近更新的 App ----\n");
        for (i = 0; i < n; i++) {
            app_label(pk_name[idx[i]], lab);
            printf(" %.10s  %s%s%s\n", pk_last[idx[i]], lab, pk_ver[idx[i]][0] ? "  v" : "", pk_ver[idx[i]]);
        }
    }
    n = top_k(nt_n, 10, key_nt, idx);
    if (n > 0) {
        printf("\n ---- 開機以來網路用量最多 (Wi-Fi + 行動網路) ----\n");
        note("上傳加下載的總量，重新開機後會歸零");
        for (i = 0; i < n; i++) {
            bytes_text(nt[idx[i]].rx + nt[idx[i]].tx, a);
            uid_num_label(nt[idx[i]].uid, lab);
            printf(" %2d. %-9s %s\n", i + 1, a, lab);
        }
    }
}

/* 使用時間頁 */
void page_usage(void)
{
    char a[48], lab[160];
    int i, n, idx[15];
    long total = 0;
    printf("\n========== 使用時間 ==========\n");
    if (us_n == 0) { printf(" 這支手機的報告沒有記錄 App 使用時間\n"); return; }
    for (i = 0; i < us_n; i++) total += us[i].sec;
    if (us_range[0]) printf(" 統計期間 %s\n", us_range);
    sec_text2(total, a);
    printf(" 所有 App 合計 %s\n\n", a);
    n = top_k(us_n, 15, key_us, idx);
    for (i = 0; i < n; i++) {
        sec_text2(us[idx[i]].sec, a);
        app_label(us[idx[i]].pkg, lab);
        printf(" %2d. %-14s %s\n", i + 1, a, lab);
    }
}

/* 耗電分析頁：耗電排行、不讓手機休眠的元件、叫醒手機的原因、鬧鐘 */
void page_power(void)
{
    char a[48], lab[160];
    int i, j, k, n = 0, idx[15];
    printf("\n========== 耗電分析 ==========\n");

    if (xp_n > 0) {
        printf(" ---- 耗電排行 ----\n");
        if (xp_onbatt[0]) { dur_text(xp_onbatt, a); printf(" 統計期間：用電池 %s", a); }
        if (xp_screen[0]) { dur_text(xp_screen, a); printf("，螢幕亮著 %s", a); }
        if (xp_onbatt[0] || xp_screen[0]) printf("\n");
        for (k = 0; k < 10; k++) {
            int best = -1;
            for (i = 0; i < xp_n; i++) {
                for (j = 0; j < n; j++) if (idx[j] == i) break;
                if (j < n) continue;
                if (best < 0 || xp[i].mah > xp[best].mah) best = i;
            }
            if (best < 0 || xp[best].mah <= 0) break;
            idx[n++] = best;
        }
        for (i = 0; i < n; i++) {
            uid_label(xp[idx[i]].uid, lab);
            printf(" %2d. %6.0f mAh  %s\n", i + 1, xp[idx[i]].mah, lab);
        }
        note("mAh 是系統估算用掉的電量；Android 系統和系統核心排前面是正常的");
    }

    if (wl_n > 0) {
        printf("\n ---- 不讓手機休眠最久的 ----\n");
        printf(" (App 或系統要求「先別睡」，時間越長越耗電)\n");
        for (i = 0; i < wl_n; i++) {
            dur_text(wl[i].dur, a);
            uid_label(wl[i].who, lab);
            printf(" %2d. %s，%ld 次  %s\n", i + 1, a, wl[i].times, lab);
            printf("       %.70s\n", wl[i].name);
        }
    }

    if (wr_n > 0) {
        printf("\n ---- 叫醒手機最多次的原因 ----\n");
        note("手機休眠時被硬體叫醒的原因，次數很多會讓待機耗電變快");
        for (i = 0; i < wr_n; i++)
        {
            printf(" %2d. %6ld 次  %s\n", i + 1, wr[i].times, wake_reason_cn(wr[i].name));
            printf("       %.60s\n", wr[i].name);
        }
    }

    n = top_k(al_n, 10, key_al, idx);
    if (n > 0) {
        printf("\n ---- 用鬧鐘叫醒手機最多次的 App ----\n");
        for (i = 0; i < n; i++) {
            app_label(al[idx[i]].pkg, lab);
            printf(" %2d. %6ld 次  %s\n", i + 1, al[idx[i]].wakes, lab);
        }
    }
    if (xp_n == 0 && wl_n == 0 && wr_n == 0 && al_n == 0) printf(" 沒有記錄\n");
}

/* 充電紀錄頁 */
void page_charge(void)
{
    char a[48], b[48];
    int i, n, idx[3];
    printf("\n========== 充電紀錄 ==========\n");
    if (bh_first[0] == '\0') { printf(" 沒有記錄\n"); return; }
    printf(" 紀錄期間 %s ～ %s\n", bh_first, bh_lastst);
    if (bh_dis_sec > 1800) {
        sec_text2((long)bh_dis_sec, a);
        printf(" 沒插電的時間 %s，累計用掉 %ld%% 的電", a, bh_drop);
        if (bh_drop >= 150) printf(" (約 %.1f 次充滿的量)", bh_drop / 100.0);
        printf("\n");
        printf(" 平均每小時掉 %.1f%%\n", bh_drop / (bh_dis_sec / 3600.0));
    }
    n = top_k(dh_n, 3, key_dh, idx);
    if (n > 0) {
        printf("\n ---- 掉電最快的時段 ----\n");
        for (i = 0; i < n; i++) printf(" %s 點那一小時  掉了 %d%%\n", dh[idx[i]].key, dh[idx[i]].drop);
    }
    note("掉電最快的時段通常是在玩遊戲、看影片、開導航，或訊號很差的時候");
    printf("\n ---- 最近幾次充電 ----\n");
    if (cs_n == 0) printf(" 這段期間沒有充電\n");
    for (i = cs_n - 1; i >= 0; i--) {
        const char *pl = strcmp(cs[i].plug, "ac") == 0 ? "插座" : strcmp(cs[i].plug, "usb") == 0 ? "USB" :
                         strcmp(cs[i].plug, "wireless") == 0 ? "無線" : cs[i].plug;
        sec_text2(cs[i].sec, a);
        sprintf(b, "%d%% → %d%%", cs[i].from, cs[i].to);
        printf(" %s  %-12s 充了 %s %s\n", cs[i].start, b, a, pl);
    }
}

/* 權限使用頁 */
void page_perm(void)
{
    char lab[160];
    int c, i, k, any = 0, shown, used[1200];
    printf("\n========== 權限使用 ==========\n");
    printf(" (最近用過這些權限的 App，新的在前面)\n");
    note("系統 App（名字是 com.android、com.miui、com.samsung 等開頭的）用位置、聯絡人是正常的，要留意的是你不認得的 App");
    for (c = 0; c < perm_cat_n; c++) {
        shown = 0;
        for (i = 0; i < pu_n; i++) used[i] = 0;
        for (k = 0; k < 8; k++) {
            int best = -1;
            for (i = 0; i < pu_n; i++) {
                if (pu[i].cat != c || used[i] || pu[i].t[0] == '\0') continue;
                if (best < 0 || strcmp(pu[i].t, pu[best].t) > 0) best = i;
            }
            if (best < 0) break;
            used[best] = 1;
            if (shown == 0) printf("\n ---- %s ----\n", perm_cn[c]);
            app_label(pu[best].pkg, lab);
            printf(" %.11s  %s\n", pu[best].t + 5, lab);
            shown++;
            any = 1;
        }
    }
    if (!any) printf("\n 沒有記錄\n");
}

/* 定位頁 */
void page_gnss(void)
{
    printf("\n========== 定位 (GPS) ==========\n");
    if (gn_reports < 0) { printf(" 沒有記錄\n"); return; }
    printf(" (開機以來的統計，不含位置座標)\n");
    printf(" 定位次數       %ld 次", gn_reports);
    if (gn_fail >= 0) printf("，失敗 %.1f%%", gn_fail);
    printf("\n");
    if (gn_ttff > 0 && gn_ttff_n > 0) printf(" 首次定位平均   %.1f 秒%s\n", gn_ttff, gn_ttff > 30 ? "  (偏慢)" : "");
    if (gn_acc > 0) printf(" 平均準確度     %.0f 公尺\n", gn_acc);
    if (gn_cn0 > 0) printf(" 衛星訊號平均   %.0f dB-Hz %s\n", gn_cn0, gn_cn0 >= 30 ? "(良好)" : gn_cn0 >= 20 ? "(普通)" : "(偏弱)");
    if (gn_l5 >= 0) printf(" 雙頻定位 (L5)  %s\n", gn_l5 > 0 ? "支援，有在使用" : "這段期間沒用到");
    if (gn_const[0]) printf(" 用到的衛星系統 %s\n", gn_const);
    note("首次定位是打開定位後多快找到位置；dB-Hz 是衛星訊號強度，30 以上算好");
    note("L5 雙頻定位在高樓多的地方比較準；GPS 是美國、GLONASS 俄羅斯、BEIDOU 中國、GALILEO 歐洲、QZSS 日本的衛星");
}

/* ================= 第 6 部分：安全、設定、效能、通知、進階數據 ================= */

/* 白話說明（App 的一般模式會藏起來，專業模式才顯示） */
void note(const char *s) { printf(" ※ %s\n", s); }

double key_nf(int i)  { return (double)nf[i].n; }
double key_cam(int i) { return (double)cam[i].n; }

/* 安全檢查頁 */
void page_security(void)
{
    char lab[160], a11y[512], *p, *q;
    int i;
    printf("\n========== 安全檢查 ==========\n");

    if (sc_lock[0]) {
        const char *t = sc_lock;
        if (strcmp(sc_lock, "PIN") == 0) t = "PIN 數字密碼";
        else if (strcmp(sc_lock, "PASSWORD") == 0) t = "文字密碼";
        else if (strcmp(sc_lock, "PATTERN") == 0) t = "圖形解鎖";
        else if (strcmp(sc_lock, "NONE") == 0) t = "沒有設定  [注意]";
        printf(" 螢幕鎖         %s\n", t);
    }
    if (sc_fp_n >= 0) printf(" 指紋           登錄了 %d 個，解鎖通過 %d 次、失敗 %d 次\n", sc_fp_n, sc_fp_ok, sc_fp_ng);
    if (sc_face_n >= 0) printf(" 臉部辨識       %s\n", sc_face_n > 0 ? "已登錄" : "沒有登錄");
    if (sc_face_n > 0) printf("                解鎖通過 %d 次、失敗 %d 次\n", sc_face_ok, sc_face_ng);
    note("通過、失敗次數是系統從上次重開機以來的統計");

    printf("\n ---- 開了「無障礙服務」的 App ----\n");
    if (sc_a11y[0] == '\0' || strcmp(sc_a11y, "-") == 0) printf(" 沒有\n");
    else {
        strcpy(a11y, sc_a11y);
        p = a11y;
        while (p && *p) {
            q = strchr(p, ':');
            if (q != NULL) *q = '\0';
            if (strchr(p, '/') != NULL) *strchr(p, '/') = '\0';
            app_label(p, lab);
            printf(" %s  [注意]\n", lab);
            p = q ? q + 1 : NULL;
        }
    }
    note("無障礙服務可以看到螢幕上所有內容、替你點擊和輸入，詐騙或惡意 App 常要求這個權限。不認得的請到「設定 → 無障礙」關掉");

    printf("\n ---- 裝置管理員 App ----\n");
    if (sc_admin_n == 0) printf(" 沒有\n");
    for (i = 0; i < sc_admin_n; i++) { app_label(sc_admin[i], lab); printf(" %s\n", lab); }
    note("裝置管理員可以鎖定、清除手機，也比較難被移除。廠商內建的（例如三星 KnoxGuard 分期付款鎖）是正常的");

    printf("\n ---- 可以「安裝未知來源 App」的 App ----\n");
    if (sc_install_n == 0) printf(" 沒有\n");
    for (i = 0; i < sc_install_n; i++) { app_label(sc_install[i], lab); printf(" %s\n", lab); }
    note("這些 App 可以自己下載並安裝其他 App。瀏覽器、檔案管理、應用商店有這個權限是常見的，其他的要留意");

    printf("\n ---- 網路安全 ----\n");
    if (sc_dns_mode[0]) {
        if (strcmp(sc_dns_mode, "hostname") == 0) printf(" 私人 DNS      指定 %s\n", sc_dns_host);
        else if (strcmp(sc_dns_mode, "off") == 0) printf(" 私人 DNS      關閉\n");
        else printf(" 私人 DNS      自動\n");
        note("私人 DNS 會把查詢網址的過程加密，避免被偷看或竄改");
    }
    printf(" 永遠開啟的 VPN %s\n", sc_vpn[0] ? sc_vpn : "沒有設定");
    if (sc_unknown_src >= 0) printf(" 舊版「未知來源」總開關  %s\n", sc_unknown_src ? "開啟" : "關閉");
    printf(" USB 偵錯       %s\n", g_adb == 1 ? "開啟" : "關閉");
    printf(" 無線偵錯       %s\n", g_adb_wifi == 1 ? "開啟  [注意]" : "關閉");
    printf(" Bootloader     %s\n", strcmp(g_locked, "1") == 0 ? "已上鎖" : "未上鎖  [注意]");
    note("Bootloader 解鎖代表可以刷入非官方系統，手機被偷時資料比較不安全");
}

/* 設定狀態頁 */
void page_settings(void)
{
    char lab[160];
    int i;
    printf("\n========== 設定狀態 ==========\n");
    if (st_bright >= 0) printf(" 螢幕亮度       %d%s\n", st_bright, st_bright_auto == 1 ? " (自動亮度開啟)" : st_bright_auto == 0 ? " (自動亮度關閉)" : "");
    note("亮度的數字範圍每家不一樣（常見是 0～255），只能和同一支手機比");
    if (st_timeout > 0) {
        if (st_timeout >= 60000) printf(" 自動關閉螢幕   %ld 分鐘\n", st_timeout / 60000);
        else printf(" 自動關閉螢幕   %ld 秒\n", st_timeout / 1000);
    }
    if (st_font > 0) printf(" 字體大小       %.0f%%\n", st_font * 100);
    if (st_night >= 0) printf(" 深色模式       %s\n", st_night == 2 ? "開啟" : st_night == 1 ? "關閉" : st_night == 0 ? "自動" : "自訂時間");
    if (st_anim >= 0 || st_trans >= 0 || st_win >= 0)
        printf(" 動畫速度       %.1fx / %.1fx / %.1fx\n", st_win < 0 ? 1 : st_win, st_trans < 0 ? 1 : st_trans, st_anim < 0 ? 1 : st_anim);
    note("動畫速度是「視窗 / 轉場 / 動畫時間」的倍率，0.5x 會感覺比較快，1.0x 是預設");
    if (st_low_power >= 0) printf(" 省電模式       %s\n", st_low_power ? "開啟" : "關閉");
    if (st_zen >= 0) printf(" 勿擾模式       %s\n", st_zen == 0 ? "關閉" : st_zen == 1 ? "只收重要通知" : st_zen == 2 ? "完全靜音" : "只有鬧鐘");

    printf("\n ---- 不受電池最佳化限制的 App ----\n");
    if (st_white_n == 0) printf(" 沒有記錄\n");
    for (i = 0; i < st_white_n; i++) { app_label(st_white[i], lab); printf(" %s\n", lab); }
    note("這些 App 在背景比較不會被省電機制限制。LINE、Messenger 推播常延遲的話，可以把它們加進來");
}

/* 效能頁 */
void page_perf(void)
{
    char a[32], lab[160];
    int i;
    printf("\n============ 效能 ============\n");
    if (mi_total > 0) {
        printf(" ---- 記憶體分配 (抓取當下) ----\n");
        bytes_text(mi_total * 1024.0, a); printf(" 總共       %s%s%s\n", a, mi_status[0] ? "  狀態 " : "", mi_status);
        if (mi_used >= 0) { bytes_text(mi_used * 1024.0, a); printf(" 使用中     %s\n", a); }
        if (mi_free >= 0) { bytes_text(mi_free * 1024.0, a); printf(" 可釋放     %s\n", a); }
        if (mi_lost > 0)  { bytes_text(mi_lost * 1024.0, a); printf(" 無法統計   %s\n", a); }
        if (mi_zram > 0)  {
            bytes_text(mi_zram * 1024.0, a);
            printf(" 壓縮記憶體 實際佔用 %s", a);
            if (mi_swap_used > 0) { bytes_text(mi_swap_used * 1024.0, a); printf("，存了 %s 的資料", a); }
            printf("\n");
        }
        note("「可釋放」是快取，需要時系統會自動清掉，所以數字小不代表不夠用");
        note("壓縮記憶體 (ZRAM) 把不常用的資料壓縮起來，用處理器換空間；狀態 low 代表記憶體比較緊");
    }
    if (pm_n > 0) {
        printf("\n ---- 佔用記憶體最多的程式 ----\n");
        for (i = 0; i < pm_n; i++) {
            bytes_text(pm[i].val * 1024.0, a);
            if (strcmp(pm[i].name, "system") == 0) strcpy(lab, "Android 系統");
            else app_label(pm[i].name, lab);
            printf(" %2d. %-8s %s\n", i + 1, a, lab);
        }
        note(strcmp(pm_title, "RSS") == 0 ? "RSS 是程式實際佔用的記憶體，共用的部分會重複計算" : "PSS 是程式實際佔用的記憶體，共用的部分按比例分攤");
    }
    if (pc_n > 0) {
        printf("\n ---- 抓取當下最吃處理器的程式 ----\n");
        if (pc_load[0]) printf(" 系統負載 %s\n", pc_load);
        for (i = 0; i < pc_n; i++) {
            app_label(pc[i].name, lab);
            printf(" %2d. %5.1f%%  %s\n", i + 1, pc[i].val, lab);
        }
        note("百分比是這幾秒內佔用的處理器，多核心手機加起來可以超過 100%");
        note("系統負載是過去 1 / 5 / 15 分鐘平均有多少工作在排隊，和核心數差不多算正常");
    }
}

/* 通知與相機頁 */
void page_notify(void)
{
    char lab[160];
    int i, n, idx[10], total = 0;
    printf("\n========= 通知與相機 =========\n");
    printf(" ---- 通知欄裡的通知 (抓取當下) ----\n");
    for (i = 0; i < nf_n; i++) total += nf[i].n;
    if (total == 0) printf(" 沒有\n");
    else printf(" 共 %d 則\n", total);
    n = top_k(nf_n, 10, key_nf, idx);
    for (i = 0; i < n; i++) { app_label(nf[idx[i]].name, lab); printf(" %3d 則  %s\n", nf[idx[i]].n, lab); }
    note("只統計數量，不會顯示通知內容");

    printf("\n ---- 最近開過相機的 App ----\n");
    n = top_k(cam_n, 10, key_cam, idx);
    if (n == 0) printf(" 沒有記錄\n");
    for (i = 0; i < n; i++) { app_label(cam[idx[i]].name, lab); printf(" %3d 次  %s\n", cam[idx[i]].n, lab); }
    note("臉部解鎖也會開前鏡頭，所以解鎖程式（例如 com.miui.face）次數多是正常的");
}

/* 進階數據頁（專業） */
void page_advanced(void)
{
    char a[48];
    int i;
    long total = 0;
    printf("\n========== 進階數據 ==========\n");
    if (cf_n > 0) {
        printf(" ---- 第一組處理器核心各頻率的時間 ----\n");
        for (i = 0; i < cf_n; i++) total += cf_t[i];
        for (i = 0; i < cf_n; i++) {
            if (total <= 0 || cf_t[i] * 1000 / total < 5) continue;       /* 不到 0.5% 的不列 */
            printf(" %5ld MHz  %4.1f%%\n", cf_f[i] / 1000, cf_t[i] * 100.0 / total);
        }
        note("處理器會依工作量自動調頻率，低頻率時間越多越省電。不到 0.5% 的頻率不列出");
    }
    if (kw_n > 0) {
        printf("\n ---- 核心喚醒鎖 (Kernel wakelock) ----\n");
        for (i = 0; i < kw_n; i++) {
            dur_text(kw[i].dur, a);
            printf(" %2d. %-14s %ld 次  %.50s\n", i + 1, a, kw[i].times, kw[i].name);
        }
        note("這是驅動程式層級「先別睡」的要求，比 App 的喚醒鎖更底層。Wi-Fi、行動網路、藍牙的名字常出現在這裡");
    }
    if (tz_n > 0) {
        printf("\n ---- 所有溫度感測器 (最近一次通知的值) ----\n");
        for (i = 0; i < tz_n; i++) printf(" %-16s %.1f°C\n", tz[i].name, tz[i].v);
        note("這是系統最近一次記錄到的溫度，不一定是抓報告當下的溫度");
    }
    if (cf_n == 0 && kw_n == 0 && tz_n == 0) printf(" 沒有記錄\n");
}

/* 原始資料頁（手機版另外做成可以點開的清單，這裡只列區段數量） */
void page_raw_summary(void)
{
    printf("\n========== 原始資料 ==========\n");
    printf(" 報告一共有 %d 個區段\n", rs_n);
    note("專業模式可以點開每個區段看原文。原文已遮掉常見的個人資料，但仍可能有漏網之魚，請不要截圖分享");
}

/* ================= 主程式 ================= */

/* 選到的是不是 zip */
int is_zip(const char *path)
{
    int n = strlen(path);
    return n > 4 && (strcmp(path + n - 4, ".zip") == 0 || strcmp(path + n - 4, ".ZIP") == 0);
}

/* 解壓縮 zip，再找出主要的 txt 檔，路徑放回 path */
int unzip_and_find(char *path)
{
    char folder[1000], cmd[2200], name[300], full[1300];
    DIR *d;
    struct dirent *e;
    struct stat info;
    double best = -1;
    int from_entry = 0;
    FILE *f;

    /* 解壓縮到 zip 旁邊、同名的資料夾 */
    strcpy(folder, path);
    folder[strlen(folder) - 4] = '\0';

    if (stat(folder, &info) == 0 && S_ISDIR(info.st_mode)) {
        printf("這個 zip 之前解壓縮過了，直接使用\n");
    } else {
        printf("正在解壓縮，大約需要 10 ～ 30 秒...\n");
        sprintf(cmd, "mkdir \"%s\"", folder);
        system(cmd);
        /* Windows 10 / 11 內建的 tar 可以解 zip */
        sprintf(cmd, "tar -xf \"%s\" -C \"%s\"", path, folder);
        if (system(cmd) != 0) {
            /* tar 失敗就改用 PowerShell */
            sprintf(cmd, "powershell -NoProfile -Command \"Expand-Archive -LiteralPath '%s' -DestinationPath '%s' -Force\"", path, folder);
            if (system(cmd) != 0) {
                printf("解壓縮失敗\n");
                return 0;
            }
        }
        printf("解壓縮完成\n");
    }

    /* 先看 main_entry.txt，它會寫主檔案的名字（三星是 dumpstate.txt） */
    name[0] = '\0';
    sprintf(full, "%s" SEP "main_entry.txt", folder);
    f = fopen(full, "rb");
    if (f != NULL) {
        if (fgets(name, sizeof(name), f) == NULL) name[0] = '\0';
        trim(name);
        fclose(f);
        sprintf(full, "%s" SEP "%s", folder, name);
        if (name[0] == '\0' || stat(full, &info) != 0) name[0] = '\0';
    }
    from_entry = (name[0] != '\0');

    /* 沒有的話，找最大的 bugreport-....txt */
    d = opendir(folder);
    if (d == NULL) return 0;
    while (!from_entry && (e = readdir(d)) != NULL) {
        int n = strlen(e->d_name);
        if (starts_with(e->d_name, "bugreport-") && n > 4 && strcmp(e->d_name + n - 4, ".txt") == 0) {
            sprintf(full, "%s" SEP "%s", folder, e->d_name);
            if (stat(full, &info) == 0 && (double)info.st_size > best) {
                best = (double)info.st_size;
                strcpy(name, e->d_name);
            }
        }
    }
    closedir(d);
    if (name[0] == '\0') {
        printf("zip 裡找不到 bugreport 的 txt 檔\n");
        return 0;
    }
    sprintf(path, "%s" SEP "%s", folder, name);
    strcpy(g_base, folder);
    g_has_dir = 1;
    return 1;
}

/* 分析一個檔案（zip 或 txt），電腦版和手機版共用 */
int run_analysis(char *path)
{
    char imp_path[1000];
    char *p;
    int i;
    FILE *test;
    struct stat info;

    if (is_zip(path)) {
        if (!unzip_and_find(path)) return 0;
    } else {
        /* 直接選 txt：如果旁邊有 FS 資料夾（之前解壓縮過），一樣讀歷史紀錄 */
        char fs[1100];
        strcpy(g_base, path);
        p = strrchr(g_base, SEP[0]);
        if (p != NULL) *p = '\0';
        sprintf(fs, "%s" SEP "FS", g_base);
        if (stat(fs, &info) == 0 && S_ISDIR(info.st_mode)) g_has_dir = 1;
    }
    printf("\n完整路徑:%s\n", path);

    test = fopen(path, "rb");
    if (test == NULL) { printf("打開失敗\n"); return 0; }
    fclose(test);
    printf("檔案已打開!\n");

    /* important.txt 放在 log 檔同一個資料夾 */
    strcpy(imp_path, path);
    p = strrchr(imp_path, SEP[0]);
    if (p != NULL) strcpy(p + 1, "important.txt");
    else strcpy(imp_path, "important.txt");
    imp = fopen(imp_path, "w");
    if (imp != NULL) fprintf(imp, "=== Errors (E/F level, not in blacklist) ===\n");

    if (read_file(path) < 0) { printf("讀取失敗\n"); return 0; }

    if (imp != NULL) {
        if (g_nunknown > 0) {
            fprintf(imp, "\n=== Unknown kill reasons ===\n");
            for (i = 0; i < g_nunknown; i++) fprintf(imp, "%s\n", unknown_reason[i]);
        }
        fclose(imp);
    }

    /* 各廠商整理自己的資料 */
    xiaomi_finish();         /* 廠商 */
    samsung_finish();        /* 廠商 */
    oppo_finish();           /* 廠商 */
    vivo_finish();           /* 廠商 */
    ex_finish();             /* 更多資料 */
    android_finish();        /* 一定要放在所有廠商後面 */

    load_history();
    judge();
    return 1;
}

#ifndef __ANDROID__
/* ================= 電腦版：選檔案 + 選單 ================= */
int main()
{
    char path[1000];
    int choice;

    if (!select_file(path)) return 1;
    if (!run_analysis(path)) return 1;

    while (1) {
        printf("\n========== 手機健檢 ==========\n");
        printf(" 型號: %s   Android %s\n", or_none(g_model), or_none(g_android));
        printf(" 抓取時間: %s\n", or_none(g_capture));
        printf("--------------------------------\n");
        printf(" 1. 總覽\n");
        printf(" 2. 系統          %s\n", status(0));
        printf(" 3. 處理器與螢幕  %s\n", status(1));
        printf(" 4. 零件與規格\n");
        printf(" 5. 記憶體        %s\n", status(2));
        printf(" 6. 儲存空間      %s\n", status(3));
        printf(" 7. 電池          %s\n", status(4));
        printf(" 8. 網路          %s\n", status(5));
        printf(" 9. 背景凍結      %s\n", status(6));
        printf("10. 穩定性        %s\n", status(7));
        printf("11. 錯誤訊息      %s\n", status(8));
        printf("12. 歷史紀錄      %s\n", g_has_hist ? status(9) : "(要選 zip 才有)");
        printf("13. 溫度          %s\n", status(10));
        printf("14. 感測器\n");
        printf("15. 音訊與震動\n");
        printf("16. 藍牙\n");
        printf("17. 應用程式\n");
        printf("18. 使用時間\n");
        printf("19. 耗電分析\n");
        printf("20. 充電紀錄\n");
        printf("21. 權限使用\n");
        printf("22. 定位 (GPS)\n");
        printf("23. 安全檢查      %s\n", status(11));
        printf("24. 設定狀態\n");
        printf("25. 效能\n");
        printf("26. 通知與相機\n");
        printf("27. 進階數據\n");
        printf(" 0. 離開\n");
        printf("請選擇:");
        if (scanf("%d", &choice) != 1) break;

        if (choice == 0) break;
        else if (choice == 1) page_overview();
        else if (choice == 2) page_system();
        else if (choice == 3) page_cpu();
        else if (choice == 4) page_parts();
        else if (choice == 5) page_memory();
        else if (choice == 6) page_storage();
        else if (choice == 7) page_battery();
        else if (choice == 8) page_network();
        else if (choice == 9) page_freeze();
        else if (choice == 10) page_stability();
        else if (choice == 11) page_errors();
        else if (choice == 12) page_history();
        else if (choice == 13) page_thermal();
        else if (choice == 14) page_sensors();
        else if (choice == 15) page_audio();
        else if (choice == 16) page_bluetooth();
        else if (choice == 17) page_apps();
        else if (choice == 18) page_usage();
        else if (choice == 19) page_power();
        else if (choice == 20) page_charge();
        else if (choice == 21) page_perm();
        else if (choice == 22) page_gnss();
        else if (choice == 23) page_security();
        else if (choice == 24) page_settings();
        else if (choice == 25) page_perf();
        else if (choice == 26) page_notify();
        else if (choice == 27) page_advanced();
        else { printf("沒有這個選項\n"); continue; }
        wait_enter();
    }
    return 0;
}
#endif

#ifdef __ANDROID__
/* ================= 手機版：給 App 呼叫 ================= */
#include <jni.h>

/* 原始資料：區段名稱 → 白話說明；*priv = 1 表示這段多半是個人資料，不提供原文 */
const char *sec_key[] = {
    "SYSTEM PROPERTIES", "SYSTEM LOG", "EVENT LOG", "STATS LOG", "LAST LOGCAT", "KERNEL LOG", "LAST KMSG", "MEMORY INFO",
    "CPU INFO", "PROCRANK", "UPTIME", "KERNEL CPUFREQ", "INTERRUPTS", "LSMOD", "PROCESSES AND THREADS", "FILESYSTEMS",
    "DISK", "NETWORK", "IPTABLES", "BLOCKED", "VM TRACES", "ANR", "TOMBSTONE", "PRINTENV", "BINDER", "LIBRARY",
    "POWEROFF", "UFS", "PL_LK", "LOG STATISTICS", "PSI", "ZONEINFO", "VMSTAT", "SLAB", "PAGETYPEINFO", "BUDDYINFO",
    "batterystats", "battery", "meminfo", "cpuinfo", "package", "usagestats", "appops", "alarm", "jobscheduler",
    "power", "deviceidle", "display", "SurfaceFlinger", "window", "input", "audio", "media.camera", "camera",
    "sensorservice", "thermalservice", "bluetooth", "connectivity", "netstats", "netpolicy", "settings", "dropbox",
    "lock_settings", "fingerprint", "face", "biometric", "device_policy", "diskstats", "storaged", "vibrator", "nfc",
    "uimode", "accessibility", "activity", "procstats", "gfxinfo", "graphicsstats", "dbinfo", "permission", "role",
    "greezer", "android.hardware", "vendor."
};
const char *sec_cn[] = {
    "系統參數：型號、版本、各種開關", "系統紀錄 (logcat)：最近發生的所有事情", "事件紀錄：App 啟動、關閉等事件", "統計紀錄",
    "上一次開機的系統紀錄", "核心紀錄 (dmesg)：硬體和驅動程式的訊息", "上一次開機的核心紀錄", "記憶體 (/proc/meminfo)",
    "處理器使用率", "各程式佔用的記憶體", "開機多久、系統負載", "處理器各頻率的時間", "硬體中斷次數", "載入的驅動程式",
    "執行中的程式和執行緒", "檔案系統", "儲存空間", "網路介面和連線", "防火牆規則", "卡住的程式", "App 卡死時的程式狀態",
    "App 卡死 (ANR) 紀錄", "底層崩潰紀錄", "環境變數", "程式之間的通訊 (Binder)", "程式庫",
    "關機原因", "儲存晶片 (UFS)", "開機程式紀錄", "紀錄緩衝區統計", "資源壓力 (PSI)", "記憶體區域", "虛擬記憶體統計",
    "核心記憶體配置", "記憶體分頁", "記憶體碎片",
    "耗電統計：各 App 耗電、喚醒、充電歷史", "電池目前狀態", "各程式記憶體使用", "處理器使用率", "已安裝的 App 和權限",
    "App 使用時間", "權限使用紀錄", "鬧鐘和定時工作", "背景工作排程", "電源和休眠", "省電模式 (Doze)", "螢幕和顯示",
    "畫面合成 (SurfaceFlinger)", "視窗", "輸入裝置 (觸控、按鍵)", "音訊", "相機服務", "相機", "感測器", "溫度",
    "藍牙", "網路連線", "網路流量", "網路限制政策", "系統設定", "系統錯誤報告 (Dropbox)", "螢幕鎖", "指紋", "臉部辨識",
    "生物辨識", "裝置管理", "儲存空間用量", "儲存讀寫統計", "震動", "NFC", "介面模式 (深色模式等)", "無障礙服務",
    "App 和活動狀態", "程式執行統計", "畫面流暢度", "畫面流暢度統計", "資料庫", "權限", "預設 App 角色",
    "小米背景凍結 (Greezer)", "硬體介面 (HAL)", "廠商自己的服務"
};
const char *sec_priv[] = {
    "account", "contact", "isms", "sms", "iphonesubinfo", "telephony", "clipboard", "location", "notification",
    "wifi", "content", "credential", "people", "autofill", "carrier", "imms", "RADIO LOG", "calendar", "keychain",
    "phone", "voicemail", "dumpsys bugreport", "SEARCH", "search"
};

const char *section_note(const char *name, int *priv)
{
    int i, n = sizeof(sec_key) / sizeof(sec_key[0]), np = sizeof(sec_priv) / sizeof(sec_priv[0]);
    *priv = 0;
    for (i = 0; i < np; i++) if (strstr(name, sec_priv[i]) != NULL) { *priv = 1; break; }
    for (i = 0; i < n; i++) if (strstr(name, sec_key[i]) != NULL) return sec_cn[i];
    return "";
}

/* sections.txt：第一行是「檔案路徑<TAB>大小」，之後每行「開始<TAB>結束<TAB>種類<TAB>名稱<TAB>隱私<TAB>說明」 */
void write_sections(void)
{
    int i, priv;
    long end;
    const char *n;
    printf("%s\t%ld\n", rs_path, rs_file_size);
    for (i = 0; i < rs_n; i++) {
        end = (i + 1 < rs_n) ? rs[i + 1].start : rs_file_size;
        n = section_note(rs[i].name, &priv);
        printf("%ld\t%ld\t%c\t%s\t%d\t%s\n", rs[i].start, end, rs[i].kind, rs[i].name, priv, n);
    }
}

char g_outdir[1000];

/* 接下來 printf 的內容都寫到 outdir 裡的這個檔案 */
void print_to(const char *name)
{
    char p[1100];
    fflush(stdout);
    sprintf(p, "%s/%s", g_outdir, name);
    freopen(p, "w", stdout);
}

/* 進度條：App 每隔一下就來問讀到哪裡了 */
JNIEXPORT jdouble JNICALL
Java_com_example_ov_ReportActivity_doneBytes(JNIEnv *env, jobject thiz) { return g_prog_done; }

JNIEXPORT jdouble JNICALL
Java_com_example_ov_ReportActivity_totalBytes(JNIEnv *env, jobject thiz) { return g_prog_total; }

/* App 傳進來：檔案路徑（.zip 會先由 App 解壓縮到同名資料夾）、結果要放的資料夾
   結果：menu.txt（每行「頁名<TAB>狀態」）、p1.txt ～ p28.txt（每一頁的內容）、sections.txt（原始資料的區段） */
JNIEXPORT jint JNICALL
Java_com_example_ov_ReportActivity_analyze(JNIEnv *env, jobject thiz, jstring jpath, jstring jout)
{
    char path[1000];
    const char *s;
    int ok, i;

    s = (*env)->GetStringUTFChars(env, jpath, NULL);
    strncpy(path, s, sizeof(path) - 1); path[sizeof(path) - 1] = '\0';
    (*env)->ReleaseStringUTFChars(env, jpath, s);
    s = (*env)->GetStringUTFChars(env, jout, NULL);
    strncpy(g_outdir, s, sizeof(g_outdir) - 1); g_outdir[sizeof(g_outdir) - 1] = '\0';
    (*env)->ReleaseStringUTFChars(env, jout, s);

    print_to("log.txt");
    ok = run_analysis(path);
    if (!ok) { fflush(stdout); return 0; }

    /* menu.txt 每行：分類名稱<TAB>狀態<TAB>1 一般模式就顯示 / 2 只有專業模式顯示 */
    print_to("menu.txt");
    printf("%s\t%s\t%d\n", "總覽", "", 1);
    printf("%s\t%s\t%d\n", "系統", status(0), 1);
    printf("%s\t%s\t%d\n", "安全檢查", status(11), 1);
    printf("%s\t%s\t%d\n", "處理器與螢幕", status(1), 1);
    printf("%s\t%s\t%d\n", "零件與規格", "", 1);
    printf("%s\t%s\t%d\n", "記憶體", status(2), 1);
    printf("%s\t%s\t%d\n", "儲存空間", status(3), 1);
    printf("%s\t%s\t%d\n", "電池", status(4), 1);
    printf("%s\t%s\t%d\n", "充電紀錄", "", 1);
    printf("%s\t%s\t%d\n", "耗電分析", "", 2);
    printf("%s\t%s\t%d\n", "溫度", status(10), 1);
    printf("%s\t%s\t%d\n", "網路", status(5), 1);
    printf("%s\t%s\t%d\n", "藍牙", "", 1);
    printf("%s\t%s\t%d\n", "定位 (GPS)", "", 2);
    printf("%s\t%s\t%d\n", "感測器", "", 2);
    printf("%s\t%s\t%d\n", "音訊與震動", "", 2);
    printf("%s\t%s\t%d\n", "應用程式", "", 1);
    printf("%s\t%s\t%d\n", "使用時間", "", 1);
    printf("%s\t%s\t%d\n", "權限使用", "", 1);
    printf("%s\t%s\t%d\n", "通知與相機", "", 1);
    printf("%s\t%s\t%d\n", "設定狀態", "", 1);
    printf("%s\t%s\t%d\n", "效能", "", 2);
    printf("%s\t%s\t%d\n", "背景凍結", status(6), 2);
    printf("%s\t%s\t%d\n", "穩定性", status(7), 1);
    printf("%s\t%s\t%d\n", "錯誤訊息", status(8), 2);
    printf("%s\t%s\t%d\n", "歷史紀錄", g_has_hist ? status(9) : "(要選 zip 才有)", 1);
    printf("%s\t%s\t%d\n", "進階數據", "", 2);
    printf("%s\t%s\t%d\n", "原始資料", "", 2);

    print_to("head.txt");
    printf("%s\nAndroid %s\n抓取時間 %s\n", or_none(g_model), or_none(g_android), or_none(g_capture));

    print_to("sections.txt");
    write_sections();

    print_to("mask.txt");
    for (i = 0; i < mk_n; i++) printf("%s\n", mk[i]);

    /* 每一頁的檔名照上面 menu.txt 的順序：p1.txt、p2.txt ... */
    print_to("p1.txt"); page_overview();
    print_to("p2.txt"); page_system();
    print_to("p3.txt"); page_security();
    print_to("p4.txt"); page_cpu();
    print_to("p5.txt"); page_parts();
    print_to("p6.txt"); page_memory();
    print_to("p7.txt"); page_storage();
    print_to("p8.txt"); page_battery();
    print_to("p9.txt"); page_charge();
    print_to("p10.txt"); page_power();
    print_to("p11.txt"); page_thermal();
    print_to("p12.txt"); page_network();
    print_to("p13.txt"); page_bluetooth();
    print_to("p14.txt"); page_gnss();
    print_to("p15.txt"); page_sensors();
    print_to("p16.txt"); page_audio();
    print_to("p17.txt"); page_apps();
    print_to("p18.txt"); page_usage();
    print_to("p19.txt"); page_perm();
    print_to("p20.txt"); page_notify();
    print_to("p21.txt"); page_settings();
    print_to("p22.txt"); page_perf();
    print_to("p23.txt"); page_freeze();
    print_to("p24.txt"); page_stability();
    print_to("p25.txt"); page_errors();
    print_to("p26.txt"); page_history();
    print_to("p27.txt"); page_advanced();
    print_to("p28.txt"); page_raw_summary();
    fflush(stdout);
    return 1;
}
#endif
