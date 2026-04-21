// 独立注入器可执行文件
// 用法: injector <package_name> <so_path> [mode]
//   mode: lol (默认) | pubg
// 需要通过 su 以 root 身份运行

#include "Injector.h"
#include <cstdio>
#include <cstring>

int main(int argc, char* argv[]) {
    if (argc < 3) {
        fprintf(stderr, "用法: %s <package_name> <so_path> <lol|pubg|dfm>\n", argv[0]);
        fprintf(stderr, "  必须指定注入模式 (lol, pubg 或 dfm)\n");
        return 1;
    }

    const char* packageName = argv[1];
    const char* soPath = argv[2];

    if (argc < 4) {
        fprintf(stderr, "[Injector] 未指定注入模式, 不执行任何操作\n");
        return 0;
    }

    Injector::InjectMode mode;
    if (strcmp(argv[3], "pubg") == 0) {
        mode = Injector::MODE_PUBG;
    } else if (strcmp(argv[3], "lol") == 0) {
        mode = Injector::MODE_LOL;
    } else if (strcmp(argv[3], "dfm") == 0) {
        mode = Injector::MODE_DFM;
    } else {
        fprintf(stderr, "[Injector] 未知模式: %s (支持: lol, pubg, dfm)\n", argv[3]);
        return 1;
    }

    fprintf(stdout, "[Injector] 包名: %s\n", packageName);
    fprintf(stdout, "[Injector] SO: %s\n", soPath);
    fprintf(stdout, "[Injector] 模式: %s\n",
            mode == Injector::MODE_PUBG ? "PUBG" :
            mode == Injector::MODE_DFM  ? "DFM"  : "LOL");

    int ret = Injector::injectByPackageName(packageName, soPath, mode);
    fprintf(stdout, "[Injector] 结果: %d\n", ret);
    return ret;
}

#ifdef OBFU_ATTRS_END
OBFU_ATTRS_END
#endif
