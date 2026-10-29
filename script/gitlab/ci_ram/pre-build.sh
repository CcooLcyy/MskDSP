#!/bin/sh
# 本文件由主作业 shell 引入，描述符 8 持有编译槽，描述符 9 仅保护准入并兼容旧版作业。
ci_ram_prepare_lock() {
    # 仅首次创建时设置共享读权限；独占创建避免并发覆盖既有 inode。
    if [ ! -e "$1" ]; then
        (umask 000; set -C; : > "$1") 2>/dev/null || {
            if [ ! -f "$1" ] || [ -L "$1" ]; then
                echo "共享内存准备失败：无法创建共享锁 $1" >&2
                return 1
            fi
        }
    fi
    if [ ! -f "$1" ] || [ -L "$1" ] || [ ! -r "$1" ]; then
        echo "共享内存准备失败：共享锁必须是可读的普通文件且不能为符号链接 $1" >&2
        return 1
    fi
}

ci_ram_prepare() {
    case "${CI_PROJECT_DIR:-}" in
        /builds/lower/*|/builds/linux/*) ;;
        *) echo '共享内存准备失败：工程路径超出允许范围' >&2; return 1 ;;
    esac
    # 在子 shell 中解析物理路径，兼容 BusyBox 且不改变主作业当前目录。
    ci_ram_real=$(CDPATH= cd "$CI_PROJECT_DIR" && pwd -P) || {
        echo '共享内存准备失败：无法解析工程真实路径' >&2
        return 1
    }
    case "$ci_ram_real" in
        /builds/lower/*|/builds/linux/*) ;;
        *) echo '共享内存准备失败：工程真实路径超出允许范围' >&2; return 1 ;;
    esac
    if [ "$(stat -f -c %T "$CI_PROJECT_DIR")" != tmpfs ] || [ "$(stat -f -c %T /builds)" != tmpfs ]; then
        echo '共享内存准备失败：工作区不是 tmpfs，拒绝回退磁盘' >&2
        return 1
    fi
    command -v flock >/dev/null 2>&1 || { echo '共享内存准备失败：缺少 flock' >&2; return 1; }
    echo '等待共享编译槽，同一时刻最多执行两个作业主脚本'
    ci_ram_prepare_lock /builds/.compile.lock || return 1
    # 只读打开不带 O_CREAT，兼容 sticky 目录中其他 UID 创建的锁。
    exec 9</builds/.compile.lock || return 1
    while :; do
        # 先等待旧版作业的长期锁，再在短暂互斥区内尝试两个编译槽。
        flock 9 || { echo '共享内存准备失败：无法取得编译准入锁' >&2; return 1; }
        ci_ram_acquired=false
        for ci_ram_slot in 0 1; do
            ci_ram_prepare_lock "/builds/.compile-slot-$ci_ram_slot.lock" || return 1
            exec 8<"/builds/.compile-slot-$ci_ram_slot.lock" || return 1
            if flock -n 8; then
                ci_ram_acquired=true
                break
            fi
            exec 8>&-
        done
        flock -u 9 || { echo '共享内存准备失败：无法释放编译准入锁' >&2; return 1; }
        if [ "$ci_ram_acquired" = true ]; then
            break
        fi
        # 等待期间不占用准入互斥锁，避免阻止其他作业释放或获取编译槽。
        sleep 1
    done
    exec 9>&-
    echo "已取得共享编译槽 $ci_ram_slot，作业退出时自动释放"
    ci_ram_free_kib=$(df -Pk /builds | awk 'END {print $4}')
    case "$ci_ram_free_kib" in
        ''|*[!0-9]*) echo '共享内存准备失败：无法读取剩余容量' >&2; return 1 ;;
    esac
    if [ "$ci_ram_free_kib" -lt $((256 * 1024)) ]; then
        echo '共享内存准备失败：剩余容量不足 256 MiB' >&2
        return 1
    fi
    TMPDIR="$CI_PROJECT_DIR/.ci-tmp"
    GOTMPDIR="$TMPDIR"
    mkdir -p "$TMPDIR" || return 1
    export TMPDIR GOTMPDIR
    echo "共享内存准备完成：剩余 $((ci_ram_free_kib / 1024)) MiB，临时目录位于工程工作区"
    awk '/^MemAvailable:/ {printf "宿主机可用内存诊断：%d MiB（仍受作业容器内存限制）\n", $2/1024}' /proc/meminfo
}

ci_ram_prepare || exit $?
