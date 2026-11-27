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
#define CAT_N 10
const char *cat_name[CAT_N] = {"系統", "處理器與螢幕", "記憶體", "儲存空間", "電池", "網路", "背景凍結", "穩定性", "錯誤訊息", "歷史紀錄"};
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
    if (g_warranty[0]) printf(" Knox 保固    %s\n", strcmp(g_warranty, "0") == 0 ? "正常（沒有刷過機）" : "已觸發（刷過非官方系統）");
    printf(" 抓取時間     %s\n", or_none(g_capture));
    printf(" 已開機       %s\n", or_none(g_uptime));
    printf(" 上次關機     %s%s\n", or_none(g_poweroff), g_poweroff_ok ? "" : "  [注意]");
    printf(" 開發者選項   %s\n", g_dev == 1 ? "開啟" : "關閉");
    printf(" USB 偵錯     %s\n", g_adb == 1 ? "開啟" : "關閉");
    printf(" 無線偵錯     %s%s\n", g_adb_wifi == 1 ? "開啟" : "關閉", g_adb_wifi == 1 ? "  [注意] 不用時建議關掉" : "");
}

void page_cpu(void)
{
    printf("\n======== 處理器與螢幕 ========\n");
    printf(" 處理器       %s %s %s (%d 核)\n", g_soc_maker, or_none(g_soc), soc_name(g_soc), g_cores);
    if (g_freq_max > 0) {
        printf(" 頻率範圍     %ld MHz ～ %.1f GHz (第一組核心)\n", g_freq_min / 1000, g_freq_max / 1000000.0);
        printf(" 休息時間     %ld%% 的時間在低頻率 %s\n", g_time_min * 100 / g_time_total, status(1));
    }
    printf(" 螢幕         %s %d x %d\n", g_oled ? "OLED" : "", g_w, g_h);
    printf(" 更新率       最高 %.0f Hz", g_max_hz);
    if (g_hz_setting[0]) printf("，目前設定 %s", g_hz_setting);
    printf("\n");
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

    if (g_amp[0]) {
        const char *maker = "";
        if (starts_with(g_amp, "cs")) maker = "Cirrus Logic";
        else if (starts_with(g_amp, "aw")) maker = "艾為 Awinic";
        else if (starts_with(g_amp, "tfa")) maker = "NXP";
        printf(" 喇叭放大器 %s %s\n", maker, g_amp);
    }
    if (g_wifi_chip[0]) printf(" Wi-Fi/藍牙 聯發科 %s%s\n", g_wifi_chip, g_wifi7 ? " (Wi-Fi 7)" : "");
}

void page_memory(void)
{
    int i;
    printf("\n=========== 記憶體 ===========\n");
    if (g_dram_gb > 0) printf(" 總量   %d GB\n", g_dram_gb);
    else               printf(" 總量   %d GB (系統可用 %.1f GB)\n", round_size(g_memtotal_kb / 1024.0 / 1024.0, ram_sizes, 9), g_memtotal_kb / 1024.0 / 1024.0);
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
    if (strcmp(g_sim[0], "ABSENT") == 0 && strcmp(g_sim[1], "ABSENT") == 0) printf(" SIM     未插卡\n");
    else if (g_sim[0][0] || g_sim[1][0]) printf(" SIM     卡槽 1: %s  卡槽 2: %s\n", or_none(g_sim[0]), or_none(g_sim[1]));
    else printf(" SIM     未記錄\n");
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
}

void page_stability(void)
{
    int i;
    printf("\n=========== 穩定性 ===========\n");
    printf(" App 閃退          %d 次   %s\n", g_ncrash, g_ncrash ? "[注意]" : "[正常]");
    printf(" 底層崩潰          %d 次   %s\n", g_nnative, g_nnative ? "[注意]" : "[正常]");
    printf(" 系統崩潰          %d 次   %s\n", g_nsys, g_nsys ? "[注意]" : "[正常]");
    printf(" App 卡死 (ANR)    %d 次   %s\n", g_nanr, g_nanr ? "[注意]" : "[正常]");
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
    printf(" 詳細內容已存到 important.txt\n");
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
        else { printf("沒有這個選項\n"); continue; }
        wait_enter();
    }
    return 0;
}
#endif

#ifdef __ANDROID__
/* ================= 手機版：給 App 呼叫 ================= */
#include <jni.h>

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
   結果：menu.txt（每行「頁名<TAB>狀態」）、p1.txt ～ p12.txt（每一頁的內容） */
JNIEXPORT jint JNICALL
Java_com_example_ov_ReportActivity_analyze(JNIEnv *env, jobject thiz, jstring jpath, jstring jout)
{
    char path[1000];
    const char *s;
    int ok;

    s = (*env)->GetStringUTFChars(env, jpath, NULL);
    strncpy(path, s, sizeof(path) - 1); path[sizeof(path) - 1] = '\0';
    (*env)->ReleaseStringUTFChars(env, jpath, s);
    s = (*env)->GetStringUTFChars(env, jout, NULL);
    strncpy(g_outdir, s, sizeof(g_outdir) - 1); g_outdir[sizeof(g_outdir) - 1] = '\0';
    (*env)->ReleaseStringUTFChars(env, jout, s);

    print_to("log.txt");
    ok = run_analysis(path);
    if (!ok) { fflush(stdout); return 0; }

    print_to("menu.txt");
    printf("%s\t%s\n", "總覽", "");
    printf("%s\t%s\n", "系統", status(0));
    printf("%s\t%s\n", "處理器與螢幕", status(1));
    printf("%s\t%s\n", "零件與規格", "");
    printf("%s\t%s\n", "記憶體", status(2));
    printf("%s\t%s\n", "儲存空間", status(3));
    printf("%s\t%s\n", "電池", status(4));
    printf("%s\t%s\n", "網路", status(5));
    printf("%s\t%s\n", "背景凍結", status(6));
    printf("%s\t%s\n", "穩定性", status(7));
    printf("%s\t%s\n", "錯誤訊息", status(8));
    printf("%s\t%s\n", "歷史紀錄", g_has_hist ? status(9) : "(要選 zip 才有)");

    print_to("head.txt");
    printf("%s\nAndroid %s\n抓取時間 %s\n", or_none(g_model), or_none(g_android), or_none(g_capture));

    print_to("p1.txt");  page_overview();
    print_to("p2.txt");  page_system();
    print_to("p3.txt");  page_cpu();
    print_to("p4.txt");  page_parts();
    print_to("p5.txt");  page_memory();
    print_to("p6.txt");  page_storage();
    print_to("p7.txt");  page_battery();
    print_to("p8.txt");  page_network();
    print_to("p9.txt");  page_freeze();
    print_to("p10.txt"); page_stability();
    print_to("p11.txt"); page_errors();
    print_to("p12.txt"); page_history();
    fflush(stdout);
    return 1;
}
#endif
