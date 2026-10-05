// tests/test_hid_descriptors.c —— HID 报告描述符的主机测试。
//
// 为什么必须有:描述符写错时,**编译通过、门禁全绿**,设备却在
// esp_hid 的解析器里 panic,表现是开机无限重启、屏幕一直闪。
// 本文件在主机上完整复刻 esp_hid/src/esp_hid_common.c 的 item 扫描与
// Input/Output/Feature 位数累加规则,在上板之前就能判定描述符是否合法。
//
// 复刻的关键两行(与 esp_hid_common.c 的 parse_cmd 一致):
//   cmd = item[0] & 0xFC   // 掩掉低 2 位,那是长度
//   len = item[0] & 0x03   // 0→0 字节,1→1 字节,2→2 字节,3→4 字节
//
// 已经被这个测试抓到过的三个真实错误:
//   1. 键盘 Input 段多了 5+3 bit 鼠标式填充 → Input 9 字节(应为 8)
//   2. LED 项写成 0x81(Input)而非 0x91(Output) → Input 69 bit、Output 3 bit
//   3. 16 位 Usage Max 写成 0x29(1 字节形式)→ 描述符从该处错位
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static int failures;

#define CHECK(cond)                                                     \
    do {                                                                \
        if (!(cond)) {                                                  \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);      \
            failures++;                                                 \
        }                                                               \
    } while (0)

// item 命令码(已掩掉长度位)
#define CMD_USAGE_PAGE 0x04
#define CMD_USAGE 0x08
#define CMD_REPORT_ID 0x8C
#define CMD_REPORT_SIZE 0x74
#define CMD_REPORT_COUNT 0x94
#define CMD_INPUT 0x80
#define CMD_OUTPUT 0x90
#define CMD_FEATURE 0xB0
#define CMD_COLLECTION 0xA0
#define CMD_END_COLLECTION 0xC0

typedef struct {
    int input_bits;
    int output_bits;
    int feature_bits;
    int reports;       // 顶层 Collection 个数
    int error;         // 0 = 合法
    char msg[96];
} desc_result_t;

static void parse_descriptor(const uint8_t *d, size_t len, desc_result_t *out)
{
    memset(out, 0, sizeof(*out));
    size_t i = 0;
    int step = 0;   // 0=等 Usage Page,1=等 Usage,2=等 Collection,3=Collection 内
    int depth = 0;
    int size = 0, count = 0;
    int acc_in = 0, acc_out = 0, acc_feat = 0;

    while (i < len) {
        const uint8_t cmd = d[i] & 0xFC;
        int n = d[i] & 0x03;
        if (n == 3) {
            n = 4;
        }
        if (len - i - 1 < (size_t)n) {
            snprintf(out->msg, sizeof(out->msg), "越界:idx=%zu 声称 %d 字节", i, n);
            out->error = 1;
            return;
        }

        if (step == 0) {
            if (cmd != CMD_USAGE_PAGE) {
                snprintf(out->msg, sizeof(out->msg), "idx=%zu 期望 Usage Page 得 0x%02X", i, cmd);
                out->error = 1;
                return;
            }
            size = count = 0;
            acc_in = acc_out = acc_feat = 0;
            step = 1;
        } else if (step == 1) {
            if (cmd != CMD_USAGE) {
                snprintf(out->msg, sizeof(out->msg), "idx=%zu 期望 Usage 得 0x%02X", i, cmd);
                out->error = 1;
                return;
            }
            step = 2;
        } else if (step == 2) {
            if (cmd != CMD_COLLECTION) {
                snprintf(out->msg, sizeof(out->msg), "idx=%zu 期望 Collection 得 0x%02X", i, cmd);
                out->error = 1;
                return;
            }
            if (d[i + 1] != 1) {
                snprintf(out->msg, sizeof(out->msg), "idx=%zu 不是 Application Collection", i);
                out->error = 1;
                return;
            }
            depth = 1;
            step = 3;
        } else {
            if (cmd == CMD_COLLECTION) {
                depth++;
            } else if (cmd == CMD_END_COLLECTION) {
                if (--depth == 0) {
                    out->reports++;
                    // 三类报告都必须整字节,否则 esp_hid 直接报错并 panic
                    if (acc_in & 7) {
                        snprintf(out->msg, sizeof(out->msg), "INPUT 未字节对齐:%d bit(余 %d)",
                                 acc_in, acc_in & 7);
                        out->error = 1;
                        return;
                    }
                    if (acc_out & 7) {
                        snprintf(out->msg, sizeof(out->msg), "OUTPUT 未字节对齐:%d bit(余 %d)",
                                 acc_out, acc_out & 7);
                        out->error = 1;
                        return;
                    }
                    if (acc_feat & 7) {
                        snprintf(out->msg, sizeof(out->msg), "FEATURE 未字节对齐:%d bit(余 %d)",
                                 acc_feat, acc_feat & 7);
                        out->error = 1;
                        return;
                    }
                    out->input_bits += acc_in;
                    out->output_bits += acc_out;
                    out->feature_bits += acc_feat;
                    step = 0;
                }
            } else if (cmd == CMD_REPORT_SIZE) {
                size = d[i + 1];
            } else if (cmd == CMD_REPORT_COUNT) {
                count = d[i + 1];
            } else if (cmd == CMD_INPUT) {
                acc_in += size * count;
            } else if (cmd == CMD_OUTPUT) {
                acc_out += size * count;
            } else if (cmd == CMD_FEATURE) {
                acc_feat += size * count;
            }
        }
        i += (size_t)n + 1;
    }
    if (step != 0) {
        snprintf(out->msg, sizeof(out->msg), "描述符在 Collection 中途结束");
        out->error = 1;
    }
}

// 与 main/kbmic_hid.c 保持一致。改那边必须同步改这里,门禁会比对长度与结构。
static const uint8_t s_map_keyboard[] = {
    0x05, 0x01, 0x09, 0x06, 0xA1, 0x01, 0x05, 0x07,
    0x19, 0xE0, 0x29, 0xE7, 0x15, 0x00, 0x25, 0x01,
    0x75, 0x01, 0x95, 0x08, 0x81, 0x02,
    0x95, 0x01, 0x75, 0x08, 0x81, 0x01,
    0x95, 0x06, 0x75, 0x08,
    0x15, 0x00, 0x25, 0xDD, 0x19, 0x00, 0x29, 0xDD,
    0x81, 0x00,
    0x05, 0x08, 0x19, 0x01, 0x29, 0x05,
    0x75, 0x01, 0x95, 0x05, 0x91, 0x02,
    0x75, 0x03, 0x95, 0x01, 0x91, 0x03,
    0xC0,
};

static const uint8_t s_map_consumer[] = {
    0x05, 0x0C, 0x09, 0x01, 0xA1, 0x01,
    0x15, 0x00, 0x26, 0x9D, 0x02,
    0x1A, 0x01, 0x00, 0x2A, 0x9D, 0x02,
    0x75, 0x10, 0x95, 0x01, 0x81, 0x00,
    0xC0,
};

int main(void)
{
    desc_result_t r;

    parse_descriptor(s_map_keyboard, sizeof(s_map_keyboard), &r);
    if (r.error) {
        printf("FAIL keyboard 描述符非法: %s\n", r.msg);
        failures++;
    }
    // 8 字节:1 修饰键 + 1 保留 + 6 键码。这是 boot keyboard 的固定尺寸,
    // 主机端按 8 字节解析,多一字节少一字节都不行。
    CHECK(r.input_bits == 64);
    CHECK(r.output_bits == 8);   // 5 bit LED + 3 bit 填充
    CHECK(r.reports == 1);

    parse_descriptor(s_map_consumer, sizeof(s_map_consumer), &r);
    if (r.error) {
        printf("FAIL consumer 描述符非法: %s\n", r.msg);
        failures++;
    }
    CHECK(r.input_bits == 16);   // 16 位 usage,装进 2 字节
    CHECK(r.reports == 1);

    // 回归:这两个畸形描述符必须被本测试拒绝,否则说明校验本身失效了。
    {
        // 键盘 + 5+3 bit 鼠标式填充 -> Input 9 字节
        static const uint8_t bad[] = {
            0x05, 0x01, 0x09, 0x06, 0xA1, 0x01, 0x05, 0x07,
            0x75, 0x01, 0x95, 0x08, 0x81, 0x02,
            0x95, 0x05, 0x75, 0x01, 0x81, 0x03,   // 多余 5 bit
            0xC0,
        };
        parse_descriptor(bad, sizeof(bad), &r);
        CHECK(r.error == 1);
    }
    {
        // 16 位 Usage Max 误用 1 字节形式 0x29。
        // 注意它**不会**让解析器报错:0x29 只吃掉 0x9D,后面的 0x02 变成
        // "Usage" item,之后 Report Size / Report Count 都被错位跳过,最后
        // Report Size 停在 0,于是 Input 静默变成 0 bit —— 描述符"合法"但
        // 毫无用处。这比直接报错更难查,所以这里断言"结果与正确版本不同"。
        static const uint8_t bad[] = {
            0x05, 0x0C, 0x09, 0x01, 0xA1, 0x01,
            0x29, 0x9D, 0x02,       // 错:应为 0x2A
            0x75, 0x10, 0x95, 0x01, 0x81, 0x00,
            0xC0,
        };
        desc_result_t r_bad;
        parse_descriptor(bad, sizeof(bad), &r_bad);
        CHECK(r_bad.input_bits != 16);
    }

    // LED 项误用 0x81(Input)而非 0x91(Output):Input 多 5 bit、Output 少 5 bit。
    {
        static const uint8_t bad[] = {
            0x05, 0x01, 0x09, 0x06, 0xA1, 0x01, 0x05, 0x07,
            0x75, 0x01, 0x95, 0x08, 0x81, 0x02,
            0x75, 0x08, 0x95, 0x01, 0x81, 0x01,
            0x75, 0x08, 0x95, 0x06, 0x81, 0x00,
            0x75, 0x01, 0x95, 0x05, 0x81, 0x02,   // 错:LED 应为 0x91
            0xC0,
        };
        desc_result_t r_bad;
        parse_descriptor(bad, sizeof(bad), &r_bad);
        // 位数不整字节时解析器直接报错并 panic;此时 input_bits 尚未落盘,
        // 所以要看错误信息里的位数是不是 69(= 64 + 误算进 Input 的 5 bit)。
        CHECK(r_bad.error == 1);
        CHECK(strstr(r_bad.msg, "69") != NULL);
    }

    if (failures) {
        printf("%d 项断言失败\n", failures);
        return 1;
    }
    printf("hid_descriptors: keyboard Input=8B/Output=1B, consumer Input=2B,畸形样本均被拒绝\n");
    return 0;
}
