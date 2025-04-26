/*
 ============================================================================
 Name        : clevo-indicator.c
 Author      : AqD <iiiaqd@gmail.com>
 Version     :
 Description : Ubuntu fan control indicator for Clevo laptops

 Based on http://www.association-apml.fr/upload/fanctrl.c by Jonas Diemer
 (diemer@gmx.de)

 ============================================================================

 TEST:
 gcc clevo-indicator.c -o clevo-indicator `pkg-config --cflags --libs appindicator3-0.1` -lm
 sudo chown root clevo-indicator
 sudo chmod u+s clevo-indicator

 Run as effective uid = root, but uid = desktop user (in order to use indicator).

 ============================================================================
 Auto fan control algorithm:

 The algorithm is to replace the builtin auto fan-control algorithm in Clevo
 laptops which is apparently broken in recent models such as W350SSQ, where the
 fan doesn't get kicked until both of GPU and CPU are really hot (and GPU
 cannot be hot anymore thanks to nVIDIA's Maxwell chips). It's far more
 aggressive than the builtin algorithm in order to keep the temperatures below
 60°C all the time, for maximized performance with Intel turbo boost enabled.

 ============================================================================
 */

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <csignal>
#include <sys/io.h>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <libayatana-appindicator3-0.1/libayatana-appindicator/app-indicator.h>
#include <nlohmann/json.hpp>
#include <fstream>

#define NAME "clevo-indicator"

#define EC_SC 0x66
#define EC_DATA 0x62

#define IBF 1
#define OBF 0
#define EC_SC_READ_CMD 0x80

/* EC registers can be read by EC_SC_READ_CMD or /sys/kernel/debug/ec/ec0/io:
 *
 * 1. modprobe ec_sys
 * 2. od -Ax -t x1 /sys/kernel/debug/ec/ec0/io
 */

#define EC_REG_SIZE 0x100
#define EC_REG_CPU_TEMP 0x07
#define EC_REG_GPU_TEMP 0x0A
#define EC_REG_CPU_FAN_DUTY 0xCE
#define EC_REG_CPU_FAN_RPMS_HI 0xD0
#define EC_REG_CPU_FAN_RPMS_LO 0xD1
#define EC_REG_GPU_FAN_DUTY 0xCF
#define EC_REG_GPU_FAN_RPMS_HI 0xD2
#define EC_REG_GPU_FAN_RPMS_LO 0xD3

#define CPU_PORT 0x01
#define GPU_PORT 0x02

#define MAX_FAN_RPM 4400.0

using json = nlohmann::json;

typedef enum {
    NA = 0, AUTO = 1, MANUAL = 2
} MenuItemType;

// FanConfig 类，用于存储风扇配置
class FanConfig {
public:
    struct FanMapping {
        int temp;  // 温度
        int duty;  // 风扇转速
    };

    std::vector<FanMapping> cpu_fan_mappings;
    std::vector<FanMapping> gpu_fan_mappings;

    // 构造函数，初始化配置
    FanConfig() {
        // 默认 CPU 风扇配置
        cpu_fan_mappings = {
            {10, 0},    // 10°C对应0%风扇转速
            {20, 20},   // 20°C对应20%风扇转速
            {30, 25},   // 30°C对应25%风扇转速
            {40, 35},   // 40°C对应35%风扇转速
            {50, 45},   // 50°C对应45%风扇转速
            {60, 60},   // 60°C对应55%风扇转速
            {70, 75},   // 70°C对应60%风扇转速
            {80, 85},   // 80°C对应70%风扇转速
            {90, 100},   // 90°C对应100%风扇转速
        };

        // 默认 GPU 风扇配置
        gpu_fan_mappings = {
            {10, 0},    // 10°C对应0%风扇转速
            {20, 20},   // 20°C对应20%风扇转速
            {30, 25},   // 30°C对应25%风扇转速
            {40, 30},   // 40°C对应30%风扇转速
            {50, 35},   // 50°C对应35%风扇转速
            {60, 45},   // 60°C对应45%风扇转速
            {70, 60},   // 70°C对应60%风扇转速
            {80, 75},   // 80°C对应75%风扇转速
            {90, 90},   // 90°C对应90%风扇转速
            {95, 100}  // 假设100°C对应最大风扇转速
        };
    }

    // 根据当前温度计算风扇转速
    static int adjust_fan_speed(int current_temp, int current_duty, const std::vector<FanMapping> &fan_mappings)
    {
        int target_duty = 0;
        // 查找温度对应的两个区间
        for (size_t i = fan_mappings.size() - 1; i > 0; --i) {
            if (current_temp >= fan_mappings[i].temp) {
                target_duty = fan_mappings[i].duty;
                break;
            }
        }

        if (target_duty > current_duty)
            return target_duty;

        for (size_t i = 1; i < fan_mappings.size(); ++i) {
            // 当前温度超出了最高值

            // 获取当前温度区间和前一个区间
            int prev_temp = fan_mappings[i - 1].temp;
            int prev_duty = fan_mappings[i - 1].duty;
            int next_temp = fan_mappings[i].temp;

            int tmp_threshold = (next_temp + prev_temp) / 2;

            // 处理降温情况
            if (current_temp <= tmp_threshold && current_duty > prev_duty)
                return prev_duty;
        }

        return 0;
    }
};

static void main_init_share(void);
static int main_ec_worker(void);
static void main_ui_worker(int argc, char** argv);
static void main_on_sigchld(int signum);
static void main_on_sigterm(int signum);
static int main_dump_fan(void);
static int main_test_fan(int duty_percentage);
static gboolean ui_update(gpointer user_data);
static void ui_command_set_fan(long fan_duty);
static void ui_command_quit(gchar* command);
static void ui_toggle_menuitems(int fan_duty);
static void ec_on_sigterm(int signum);
static int ec_init(void);
static int ec_auto_cpu_duty_adjust(void);
static int ec_auto_gpu_duty_adjust(void);
static int ec_query_cpu_temp(void);
static int ec_query_gpu_temp(void);
static int ec_query_cpu_fan_duty(void);
static int ec_query_gpu_fan_duty(void);
static int ec_query_cpu_fan_rpms(void);
static int ec_query_gpu_fan_rpms(void);
static int ec_write_cpu_fan_duty(int duty_percentage);
static int ec_write_gpu_fan_duty(int duty_percentage);
static int ec_io_wait(const uint32_t port, const uint32_t flag,
        const char value);
static uint8_t ec_io_read(const uint32_t port);
static int ec_io_do(const uint32_t cmd, const uint32_t port,
        const uint8_t value);
static int calculate_fan_duty(int raw_duty);
static int calculate_fan_rpms(int raw_rpm_high, int raw_rpm_low);
static int check_proc_instances(const char* proc_name);
static void get_time_string(char* buffer, size_t max, const char* format);
static void signal_term(void (*handler)(int));

static AppIndicator* indicator = nullptr;

struct {
    char label[256];
    GCallback callback;
    long option;
    MenuItemType type;
    GtkWidget* widget;

}static menuitems[] = {
        { "Set FAN to AUTO", G_CALLBACK(ui_command_set_fan), 0, AUTO, nullptr },
        { "", nullptr, 0L, NA, nullptr },
        { "Set FAN to  40%", G_CALLBACK(ui_command_set_fan), 40, MANUAL, nullptr },
        { "Set FAN to  50%", G_CALLBACK(ui_command_set_fan), 50, MANUAL, nullptr },
        { "Set FAN to  60%", G_CALLBACK(ui_command_set_fan), 60, MANUAL, nullptr },
        { "Set FAN to  70%", G_CALLBACK(ui_command_set_fan), 70, MANUAL, nullptr },
        { "Set FAN to  80%", G_CALLBACK(ui_command_set_fan), 80, MANUAL, nullptr },
        { "Set FAN to  90%", G_CALLBACK(ui_command_set_fan), 90, MANUAL, nullptr },
        { "Set FAN to 100%", G_CALLBACK(ui_command_set_fan), 100, MANUAL, nullptr },
        { "", nullptr, 0L, NA, nullptr },
        { "Quit", G_CALLBACK(ui_command_quit), 0L, NA, nullptr }
};

static int menuitem_count = (sizeof(menuitems) / sizeof(menuitems[0]));

typedef struct {
    volatile int exit;
    volatile int cpu_temp;
    volatile int gpu_temp;
    volatile int cpu_fan_duty;
    volatile int cpu_fan_rpms;
    volatile int gpu_fan_duty;
    volatile int gpu_fan_rpms;
    volatile int auto_duty;
    volatile int auto_cpu_duty_val;
    volatile int auto_gpu_duty_val;
    volatile int manual_next_fan_duty;
    volatile int manual_prev_fan_duty;
} SHARE_INFO;
static SHARE_INFO* share_info = nullptr;

static pid_t parent_pid = 0;

// 全局变量
static FanConfig g_fan_config;
int fan_config_valid = 0;  // 用于标记配置是否有效

int load_config(const std::string& filename) {
    std::ifstream file(filename);
    if (!file.is_open()) {
        fprintf(stderr, "Failed to open fan config file: %s\n", filename.c_str());
        return -1;  // 文件打开失败
    }

    try {
        json j;
        file >> j;  // 解析 JSON 文件

        // 解析 CPU 配置
        for (const auto& item : j["cpu"]) {
            g_fan_config.cpu_fan_mappings.push_back({item["temp"], item["duty"]});
        }

        // 解析 GPU 配置
        for (const auto& item : j["gpu"]) {
            g_fan_config.gpu_fan_mappings.push_back({item["temp"], item["duty"]});
        }

        fan_config_valid = 1;  // 配置文件读取成功
        return 0;
    } catch (const std::exception& e) {
        fprintf(stderr, "Error reading config: %s\n", e.what());
        return -1;  // JSON 解析失败
    }
}

// 线性插值函数
int linear_interpolate(int temp, const std::vector<FanConfig::FanMapping>& config) {
    for (size_t i = 1; i < config.size(); ++i) {
        if (temp < config[i].temp) {
            // 线性插值：计算两个温度区间之间的风扇转速
            int temp_diff = config[i].temp - config[i - 1].temp;
            int duty_diff = config[i].duty - config[i - 1].duty;

            // 计算当前温度在两个温度区间之间的比例
            float ratio = static_cast<float>(temp - config[i - 1].temp) / temp_diff;

            // 返回插值计算的风扇转速
            return static_cast<int>(config[i - 1].duty + ratio * duty_diff);
        }
    }
    return config.back().duty;  // 如果温度超出了配置范围，返回最大风扇转速
}

int main(int argc, char* argv[]) {
    printf("Simple fan control utility for Clevo laptops\n");

    // 加载配置文件
    if (load_config("/etc/fan_config.json") != 0) {
        printf("Using default fan settings...\n");
        fan_config_valid = 0;  // 配置加载失败，使用默认设置
    }

    // 检查程序是否已经有实例在运行
    if (check_proc_instances(NAME) > 1) {
        printf("Multiple running instances!\n");
        char* display = getenv("DISPLAY");
        if (display != nullptr && strlen(display) > 0) {
            int desktop_uid = getuid();
            setuid(desktop_uid);
            gtk_init(&argc, &argv);
            GtkWidget* dialog = gtk_message_dialog_new(nullptr, static_cast<GtkDialogFlags>(0),
                    GTK_MESSAGE_ERROR, GTK_BUTTONS_CLOSE,
                    "Multiple running instances of %s!", NAME);
            gtk_dialog_run(GTK_DIALOG(dialog));
            gtk_widget_destroy(dialog);
        }
        return EXIT_FAILURE;
    }

    // 初始化嵌入式控制（EC）
    if (ec_init() != EXIT_SUCCESS) {
        printf("unable to control EC: %s\n", strerror(errno));
        return EXIT_FAILURE;
    }

    // 处理命令行参数
    if (argc <= 1) {
        char* display = getenv("DISPLAY");
        if (display == nullptr || strlen(display) == 0) {
            return main_dump_fan();
        } else {
            parent_pid = getpid();
            main_init_share();
            signal(SIGCHLD, &main_on_sigchld);
            signal_term(&main_on_sigterm);
            pid_t worker_pid = fork();
            if (worker_pid == 0) {
                signal(SIGCHLD, SIG_DFL);
                signal_term(&ec_on_sigterm);
                return main_ec_worker();
            } else if (worker_pid > 0) {
                main_ui_worker(argc, argv);
                share_info->exit = 1;
                waitpid(worker_pid, nullptr, 0);
            } else {
                printf("unable to create worker: %s\n", strerror(errno));
                return EXIT_FAILURE;
            }
        }
    } else {
        if (argv[1][0] == '-') {
            printf(
                    "\n\
Usage: clevo-indicator [fan-duty-percentage]\n\
\n\
Dump/Control fan duty on Clevo laptops. Display indicator by default.\n\
\n\
Arguments:\n\
  [fan-duty-percentage]\t\tTarget fan duty in percentage, from 40 to 100\n\
  -?\t\t\t\tDisplay this help and exit\n\
\n\
Without arguments this program should attempt to display an indicator in\n\
the Ubuntu tray area for fan information display and control. The indicator\n\
requires this program to have setuid=root flag but run from the desktop user\n\
, because a root user is not allowed to display a desktop indicator while a\n\
non-root user is not allowed to control Clevo EC (Embedded Controller that's\n\
responsible of the fan). Fix permissions of this executable if it fails to\n\
run:\n\
    sudo chown root clevo-indicator\n\
    sudo chmod u+s  clevo-indicator\n\
\n\
Note any fan duty change should take 1-2 seconds to come into effect - you\n\
can verify by the fan speed displayed on indicator icon and also louder fan\n\
noise.\n\
\n\
In the indicator mode, this program would always attempt to load kernel\n\
module 'ec_sys', in order to query EC information from\n\
'/sys/kernel/debug/ec/ec0/io' instead of polling EC ports for readings,\n\
which may be more risky if interrupted or concurrently operated during the\n\
process.\n\
\n\
DO NOT MANIPULATE OR QUERY EC I/O PORTS WHILE THIS PROGRAM IS RUNNING.\n\
\n");
            return main_dump_fan();
        } else {
            int val = atoi(argv[1]);
            if (val < 40 || val > 100) {
                printf("invalid fan duty %d!\n", val);
                return EXIT_FAILURE;
            }
            return main_test_fan(val);
        }
    }
    return EXIT_SUCCESS;
}

static void main_init_share(void) {
    void* shm = mmap(nullptr, 4096, PROT_READ | PROT_WRITE, MAP_ANON | MAP_SHARED,
            -1, 0);
    share_info = (SHARE_INFO *)shm;
    share_info->exit = 0;
    share_info->cpu_temp = 0;
    share_info->gpu_temp = 0;
    share_info->cpu_fan_duty = 0;
    share_info->cpu_fan_rpms = 0;
    share_info->auto_duty = 1;
    share_info->auto_cpu_duty_val = 0;
    share_info->auto_gpu_duty_val = 0;
    share_info->manual_next_fan_duty = 0;
    share_info->manual_prev_fan_duty = 0;
}

static int main_ec_worker(void) {
    setuid(0);
    system("modprobe ec_sys");
    while (share_info->exit == 0) {
        // check parent
        if (parent_pid != 0 && kill(parent_pid, 0) == -1) {
            printf("worker on parent death\n");
            break;
        }
        // write EC
        int new_fan_duty = share_info->manual_next_fan_duty;
        if (new_fan_duty != 0
                && new_fan_duty != share_info->manual_prev_fan_duty) {
            fprintf(stderr, "manual fan duty %d\n", new_fan_duty);
            ec_write_cpu_fan_duty(new_fan_duty);
            ec_write_gpu_fan_duty(new_fan_duty);
            share_info->manual_prev_fan_duty = new_fan_duty;
        }
        // read EC
        int io_fd = open("/sys/kernel/debug/ec/ec0/io", O_RDONLY, 0);
        if (io_fd < 0) {
            printf("unable to read EC from sysfs: %s\n", strerror(errno));
            exit(EXIT_FAILURE);
        }
        unsigned char buf[EC_REG_SIZE];
        ssize_t len = read(io_fd, buf, EC_REG_SIZE);
        switch (len) {
        case -1:
            printf("unable to read EC from sysfs: %s\n", strerror(errno));
            break;
        case 0x100:
            share_info->cpu_temp = buf[EC_REG_CPU_TEMP];
            share_info->gpu_temp = buf[EC_REG_GPU_TEMP];

            share_info->cpu_fan_duty = calculate_fan_duty(buf[EC_REG_CPU_FAN_DUTY]);
            share_info->cpu_fan_rpms = calculate_fan_rpms(buf[EC_REG_CPU_FAN_RPMS_HI],
                    buf[EC_REG_CPU_FAN_RPMS_LO]);

            share_info->gpu_fan_duty = calculate_fan_duty(buf[EC_REG_GPU_FAN_DUTY]);
            share_info->gpu_fan_rpms = calculate_fan_rpms(buf[EC_REG_GPU_FAN_RPMS_HI],
                    buf[EC_REG_GPU_FAN_RPMS_LO]);

            printf("## cpu_temp=%d, duty=%d, rpms=%d\n", share_info->cpu_temp,
            share_info->cpu_fan_duty, share_info->cpu_fan_rpms);

            printf("** gpu_temp=%d, duty=%d, rpms=%d\n", share_info->gpu_temp,
            share_info->gpu_fan_duty, share_info->gpu_fan_rpms);

            break;
        default:
            printf("wrong EC size from sysfs: %ld\n", len);
        }
        close(io_fd);
        // auto EC
        if (share_info->auto_duty == 1) {
            int next_cpu_duty = ec_auto_cpu_duty_adjust();
            int next_gpu_duty = ec_auto_gpu_duty_adjust();
            if (next_cpu_duty != 0 && next_cpu_duty != share_info->auto_cpu_duty_val) {
                char s_time[256];
                get_time_string(s_time, 256, "%m/%d %H:%M:%S");
                printf("%s CPU=%d°C, auto fan duty to %d%%\n", s_time,
                        share_info->cpu_temp, next_cpu_duty);
                ec_write_cpu_fan_duty(next_cpu_duty);
                share_info->auto_gpu_duty_val = next_cpu_duty;
            }
            if (next_gpu_duty != 0 && next_gpu_duty != share_info->auto_gpu_duty_val) {
                char s_time[256];
                get_time_string(s_time, 256, "%m/%d %H:%M:%S");
                printf("%s GPU=%d°C, auto fan duty to %d%%\n", s_time,
                        share_info->gpu_temp, next_gpu_duty);
                ec_write_gpu_fan_duty(next_gpu_duty);
                share_info->auto_gpu_duty_val = next_gpu_duty;
            }
        }
        //
        usleep(200 * 1000);
    }
    printf("worker quit\n");
    return EXIT_SUCCESS;
}

static void main_ui_worker(int argc, char** argv) {
    printf("Indicator...\n");
    int desktop_uid = getuid();
    setuid(desktop_uid);
    //
    gtk_init(&argc, &argv);
    //
    GtkWidget* indicator_menu = gtk_menu_new();
    for (int i = 0; i < menuitem_count; i++) {
        GtkWidget* item;
        if (strlen(menuitems[i].label) == 0) {
            item = gtk_separator_menu_item_new();
        } else {
            item = gtk_menu_item_new_with_label(menuitems[i].label);
            g_signal_connect_swapped(item, "activate",
                    G_CALLBACK(reinterpret_cast<void(*)(void)>(menuitems[i].callback)),
                    reinterpret_cast<void*>(menuitems[i].option));
        }
        gtk_menu_shell_append(GTK_MENU_SHELL(indicator_menu), item);
        menuitems[i].widget = item;
    }
    gtk_widget_show_all(indicator_menu);
    //
    indicator = app_indicator_new(NAME, "brasero",
            APP_INDICATOR_CATEGORY_HARDWARE);
    g_assert(IS_APP_INDICATOR(indicator));
    app_indicator_set_label(indicator, "Init..", "XX");
    app_indicator_set_status(indicator, APP_INDICATOR_STATUS_ATTENTION);
    app_indicator_set_ordering_index(indicator, -2);
    app_indicator_set_title(indicator, "Clevo");
    app_indicator_set_menu(indicator, GTK_MENU(indicator_menu));
    g_timeout_add(500, &ui_update, nullptr);
    ui_toggle_menuitems(share_info->cpu_fan_duty);
    gtk_main();
    printf("main on UI quit\n");
}

static void main_on_sigchld(int signum) {
    printf("main on worker quit signal\n");
    exit(EXIT_SUCCESS);
}

static void main_on_sigterm(int signum) {
    printf("main on signal: %s\n", strsignal(signum));
    if (share_info != nullptr)
        share_info->exit = 1;
    exit(EXIT_SUCCESS);
}

static int main_dump_fan(void) {
    printf("Dump fan information\n");
    printf("  CPU FAN Duty: %d%%\n", ec_query_cpu_fan_duty());
    printf("  GPU FAN Duty: %d%%\n", ec_query_gpu_fan_duty());
    printf("  CPU FAN RPMs: %d RPM\n", ec_query_cpu_fan_rpms());
    printf("  GPU FAN RPMs: %d RPM\n", ec_query_gpu_fan_rpms());
    printf("  CPU Temp: %d°C\n", ec_query_cpu_temp());
    printf("  GPU Temp: %d°C\n", ec_query_gpu_temp());
    return EXIT_SUCCESS;
}

static int main_test_fan(int duty_percentage) {
    printf("Change fan duty to %d%%\n", duty_percentage);
    ec_write_cpu_fan_duty(duty_percentage);
    ec_write_gpu_fan_duty(duty_percentage);
    printf("\n");
    main_dump_fan();
    return EXIT_SUCCESS;
}

static gboolean ui_update(gpointer user_data) {
    char label[256];
    sprintf(label, "%d℃ %d℃", share_info->cpu_temp, share_info->gpu_temp);
    app_indicator_set_label(indicator, label, "XXXXXX");
    char icon_name[256];
    double load = ((double) share_info->cpu_fan_rpms) / MAX_FAN_RPM * 100.0;
    double load_r = round(load / 5.0) * 5.0;
    sprintf(icon_name, "brasero-disc-%02d", (int) load_r);
    app_indicator_set_icon(indicator, icon_name);
    return G_SOURCE_CONTINUE;
}

static void ui_command_set_fan(long fan_duty) {
    int fan_duty_val = static_cast<int>(fan_duty);
    if (fan_duty_val == 0) {
        printf("clicked on fan duty auto\n");
        share_info->auto_duty = 1;
        share_info->auto_cpu_duty_val = 0;
        share_info->auto_gpu_duty_val = 0;
        share_info->manual_next_fan_duty = 0;
    } else {
        printf("clicked on fan duty: %d\n", fan_duty_val);
        share_info->auto_duty = 0;
        share_info->auto_cpu_duty_val = 0;
        share_info->auto_gpu_duty_val = 0;
        share_info->manual_next_fan_duty = fan_duty_val;
    }
    ui_toggle_menuitems(fan_duty_val);
}

static void ui_command_quit(gchar* command) {
    printf("clicked on quit\n");
    gtk_main_quit();
}

static void ui_toggle_menuitems(int fan_duty) {
    for (int i = 0; i < menuitem_count; i++) {
        if (menuitems[i].widget == nullptr)
            continue;
        if (fan_duty == 0)
            gtk_widget_set_sensitive(menuitems[i].widget,
                    menuitems[i].type != AUTO);
        else
            gtk_widget_set_sensitive(menuitems[i].widget,
                    menuitems[i].type != MANUAL
                            || (int) menuitems[i].option != fan_duty);
    }
}

static int ec_init(void) {
    if (ioperm(EC_DATA, 1, 1) != 0)
        return EXIT_FAILURE;
    if (ioperm(EC_SC, 1, 1) != 0)
        return EXIT_FAILURE;
    return EXIT_SUCCESS;
}

static void ec_on_sigterm(int signum) {
    printf("ec on signal: %s\n", strsignal(signum));
    if (share_info != nullptr)
        share_info->exit = 1;
}
#if 0
static int ec_auto_cpu_duty_adjust(void) {
    int temp = MAX(share_info->cpu_temp, share_info->gpu_temp);
    int duty = share_info->cpu_fan_duty;
    //
    if (temp >= 80 && duty < 100)
        return 100;
    if (temp >= 70 && duty < 90)
        return 90;
    if (temp >= 60 && duty < 80)
        return 80;
    if (temp >= 50 && duty < 70)
        return 70;
    if (temp >= 40 && duty < 60)
        return 60;
    if (temp >= 30 && duty < 50)
        return 50;
    if (temp >= 20 && duty < 40)
        return 40;
    if (temp >= 10 && duty < 30)
        return 30;
    //
    if (temp <= 15 && duty > 30)
        return 30;
    if (temp <= 25 && duty > 40)
        return 40;
    if (temp <= 35 && duty > 50)
        return 50;
    if (temp <= 45 && duty > 60)
        return 60;
    if (temp <= 55 && duty > 70)
        return 70;
    if (temp <= 65 && duty > 80)
        return 80;
    if (temp <= 75 && duty > 90)
        return 90;
    //
    return 0;
}
#else
static int ec_auto_cpu_duty_adjust(void) {
    int temp = share_info->cpu_temp;
    int duty = share_info->cpu_fan_duty;
    int target_duty = 0;

    target_duty = FanConfig::adjust_fan_speed(temp, duty, g_fan_config.cpu_fan_mappings);
    // printf("*** CPU temp: %d, duty: %d, target_duty: %d\n", temp, duty, target_duty);
    return target_duty;
}

static int ec_auto_gpu_duty_adjust(void) {
    int temp = share_info->gpu_temp;
    int duty = share_info->gpu_fan_duty;
    int target_duty = 0;

    target_duty = FanConfig::adjust_fan_speed(temp, duty, g_fan_config.gpu_fan_mappings);
    // printf("*** GPU temp: %d, duty: %d, target_duty: %d\n", temp, duty, target_duty);
    return target_duty;
}

#endif

static int ec_query_cpu_temp(void) {
    return ec_io_read(EC_REG_CPU_TEMP);
}

static int ec_query_gpu_temp(void) {
    return ec_io_read(EC_REG_GPU_TEMP);
}

static int ec_query_cpu_fan_duty(void) {
    int raw_duty = ec_io_read(EC_REG_CPU_FAN_DUTY);
    return calculate_fan_duty(raw_duty);
}

static int ec_query_gpu_fan_duty(void) {
    int raw_duty = ec_io_read(EC_REG_GPU_FAN_DUTY);
    return calculate_fan_duty(raw_duty);
}

static int ec_query_cpu_fan_rpms(void) {
    int raw_rpm_hi = ec_io_read(EC_REG_CPU_FAN_RPMS_HI);
    int raw_rpm_lo = ec_io_read(EC_REG_CPU_FAN_RPMS_LO);
    return calculate_fan_rpms(raw_rpm_hi, raw_rpm_lo);
}

static int ec_query_gpu_fan_rpms(void) {
    int raw_rpm_hi = ec_io_read(EC_REG_GPU_FAN_RPMS_HI);
    int raw_rpm_lo = ec_io_read(EC_REG_GPU_FAN_RPMS_LO);
    return calculate_fan_rpms(raw_rpm_hi, raw_rpm_lo);
}

static int ec_write_cpu_fan_duty(int duty_percentage) {
    // if (duty_percentage < 10 || duty_percentage > 100) {
    //     printf("Wrong CPU fan duty to write: %d\n", duty_percentage);
    //     return EXIT_FAILURE;
    // }
    if (duty_percentage < 10) {
        duty_percentage = 10;
    } else if (duty_percentage > 100) {
        duty_percentage = 100;
    }
    double v_d = ((double) duty_percentage) / 100.0 * 255.0;
    int v_i = (int) v_d;
    return ec_io_do(0x99, CPU_PORT, v_i);
}

static int ec_write_gpu_fan_duty(int duty_percentage) {
    // if (duty_percentage < 10 || duty_percentage > 100) {
    //     printf("Wrong GPU fan duty to write: %d\n", duty_percentage);
    //     return EXIT_FAILURE;
    // }
    if (duty_percentage < 10) {
        duty_percentage = 10;
    } else if (duty_percentage > 100) {
        duty_percentage = 100;
    }
    double v_d = ((double) duty_percentage) / 100.0 * 255.0;
    int v_i = (int) v_d;
    return ec_io_do(0x99, GPU_PORT, v_i);
}

static int ec_io_wait(const uint32_t port, const uint32_t flag,
        const char value) {
    uint8_t data = inb(port);
    int i = 0;
    while ((((data >> flag) & 0x1) != value) && (i++ < 100)) {
        usleep(1000);
        data = inb(port);
    }
    if (i >= 100) {
        printf("wait_ec error on port 0x%x, data=0x%x, flag=0x%x, value=0x%x\n",
                port, data, flag, value);
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}

static uint8_t ec_io_read(const uint32_t port) {
    ec_io_wait(EC_SC, IBF, 0);
    outb(EC_SC_READ_CMD, EC_SC);

    ec_io_wait(EC_SC, IBF, 0);
    outb(port, EC_DATA);

    //wait_ec(EC_SC, EC_SC_IBF_FREE);
    ec_io_wait(EC_SC, OBF, 1);
    uint8_t value = inb(EC_DATA);

    return value;
}

static int ec_io_do(const uint32_t cmd, const uint32_t port,
        const uint8_t value) {
    ec_io_wait(EC_SC, IBF, 0);
    outb(cmd, EC_SC);

    ec_io_wait(EC_SC, IBF, 0);
    outb(port, EC_DATA);

    ec_io_wait(EC_SC, IBF, 0);
    outb(value, EC_DATA);

    return ec_io_wait(EC_SC, IBF, 0);
}

static int calculate_fan_duty(int raw_duty) {
    return (int) ((double) raw_duty / 255.0 * 100.0);
}

static int calculate_fan_rpms(int raw_rpm_high, int raw_rpm_low) {
    int raw_rpm = (raw_rpm_high << 8) + raw_rpm_low;
    return raw_rpm > 0 ? (2156220 / raw_rpm) : 0;
}

static int check_proc_instances(const char* proc_name) {
    int proc_name_len = strlen(proc_name);
    pid_t this_pid = getpid();
    DIR* dir;
    if (!(dir = opendir("/proc"))) {
        perror("can't open /proc");
        return -1;
    }
    int instance_count = 0;
    struct dirent* ent;
    while ((ent = readdir(dir)) != nullptr) {
        char* endptr;
        long lpid = strtol(ent->d_name, &endptr, 10);
        if (*endptr != '\0')
            continue;
        if (lpid == this_pid)
            continue;
        char buf[512];
        snprintf(buf, sizeof(buf), "/proc/%ld/comm", lpid);
        FILE* fp = fopen(buf, "r");
        if (fp) {
            if (fgets(buf, sizeof(buf), fp) != nullptr) {
                if ((buf[proc_name_len] == '\n' || buf[proc_name_len] == '\0')
                        && strncmp(buf, proc_name, proc_name_len) == 0) {
                    fprintf(stderr, "Process: %ld\n", lpid);
                    instance_count += 1;
                }
            }
            fclose(fp);
        }
    }
    closedir(dir);
    return instance_count;
}

static void get_time_string(char* buffer, size_t max, const char* format) {
    time_t timer;
    struct tm tm_info;
    time(&timer);
    localtime_r(&timer, &tm_info);
    strftime(buffer, max, format, &tm_info);
}

static void signal_term(void (*handler)(int)) {
    std::signal(SIGHUP, handler);
    std::signal(SIGINT, handler);
    std::signal(SIGQUIT, handler);
    std::signal(SIGPIPE, handler);
    std::signal(SIGALRM, handler);
    std::signal(SIGTERM, handler);
    std::signal(SIGUSR1, handler);
    std::signal(SIGUSR2, handler);
}
