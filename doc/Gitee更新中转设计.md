# Gitee 更新包中转（已弃用）

## 目标

更新包发布已迁移到 Cloudflare R2，当前工作流不再使用 Gitee 或服务器 SSH 中转。本文档保留作历史记录；当前方案请参阅《GitHub Actions 与发版策略》中的 Cloudflare R2 章节。

## 历史 GitHub 配置

在仓库 Actions Secrets 中配置 `GITEE_TOKEN`。可通过 Actions Variables 覆盖 `GITEE_OWNER`、`GITEE_REPO`；默认值为 `CcooLcyy`、`mskdsp-update`。Token 只用于创建 Release 和上传附件，不能写入日志或提交到仓库。

## Release 约定

每次构建使用唯一 tag，并按顺序上传安装包、校验文件、`latest.json`。服务器将 `latest.json` 视为完整发布标志。服务器下载到临时目录后先验证 SHA256，校验成功才替换静态目录。

## 服务器配置

同步脚本部署在 `/home/daniel/update-server/relay/sync_gitee_release.py`。状态文件位于 `/home/daniel/update-server/.gitee-relay-state`，锁文件保证上位机和下位机同步不会并发覆盖。失败时保留原有版本。

## 回滚

暂时停用 Gitee 发布步骤即可恢复旧的 SCP 同步；服务器已发布文件不受影响。也可以使用 `--force` 重新拉取指定 Release。
