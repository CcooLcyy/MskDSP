#!/bin/sh
# 本文件由 Runner 的主作业 shell 引入，文件描述符 9 将持续持有共享锁。
ci_ram_prepare() {
    case "${CI_PROJECT_DIR:-}" in
        /builds/lower/*|/builds/linux/*) ;;
        *) echo '共享内存准备失败：工程路径超出允许范围' >&2; return 1 ;;
    esac
    ci_ram_real=$(realpath -e "$CI_PROJECT_DIR") || return 1
    case "$ci_ram_real" in
        /builds/lower/*|/builds/linux/*) ;;
        *) echo '共享内存准备失败：工程真实路径超出允许范围' >&2; return 1 ;;
    esac
    if [ "$(stat -f -c %T "$CI_PROJECT_DIR")" != tmpfs ] || [ "$(stat -f -c %T /builds)" != tmpfs ]; then
        echo '共享内存准备失败：工作区不是 tmpfs，拒绝回退磁盘' >&2
        return 1
    fi
    command -v flock >/dev/null 2>&1 || { echo '共享内存准备失败：缺少 flock' >&2; return 1; }
    echo '等待共享编译锁，同一时刻只执行一个作业主脚本'
    exec 9>/builds/.compile.lock || return 1
    flock 9 || { echo '共享内存准备失败：无法取得共享编译锁' >&2; return 1; }
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
