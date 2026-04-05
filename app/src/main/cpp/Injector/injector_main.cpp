// 独立注入器可执行文件
// 用法: injector <package_name> <so_path>
// 需要通过 su 以 root 身份运行

#include "Injector.h"
#include <cstdio>
#include <cstring>

int main(int argc, char* argv[]) {
    if (argc < 3) {
        fprintf(stderr, "用法: %s <package_name> <so_path>\n", argv[0]);
        return 1;
    }

    const char* packageName = argv[1];
    const char* soPath = argv[2];

    fprintf(stdout, "[Injector] 包名: %s\n", packageName);
    fprintf(stdout, "[Injector] SO: %s\n", soPath);

    int ret = Injector::injectByPackageName(packageName, soPath);
    fprintf(stdout, "[Injector] 结果: %d\n", ret);
    return ret;
}

#ifdef OBFU_ATTRS_END
OBFU_ATTRS_END
#endif
