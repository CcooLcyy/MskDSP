# 下位机 GitLab Runner

本目录提供同机独立 Runner 的部署模板。服务器使用独立目录 `/home/daniel/mskdsp-lower-ci/runner`，配置和缓存与平台 Runner 分离；项目专用标签仅为 `lower-ci`，关闭无标签作业并限制为受保护分支。

在 GitLab 下位机项目创建项目级 Runner，再用其认证令牌注册。将 `config.template.toml` 复制为服务器私有的 `config/config.toml`，替换令牌后设置权限 0600；不要提交实际配置或令牌。复制 `compose.yml` 后执行 `docker compose up -d`。

Runner 管理容器需要访问 Docker socket 以创建作业容器；作业容器不挂载该 socket，且不使用特权模式。作业通过独立 BuildKit 服务制作镜像。并发 6、单作业 8 CPU/16 GiB 与平台相同。

每个下位机作业的 `/dev/shm` 上限为 1 GiB，计入上述内存限制。完整测试仅将临时写入工作目录放入该内存文件系统，种子模块仍从磁盘读取；避免共享磁盘归档干扰 SQLite/SOE 单测的既有等待预算。

宿主机须已注册 ARM64 QEMU。准备流程及验收要求见 `doc/GitLab-CI接入方案.md`；工具镜像升级时应使用新标签，保留现有平台工具镜像。
