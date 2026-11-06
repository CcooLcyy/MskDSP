"""将 GitHub 原生运行身份转换为共享 CI 元数据约定，不伪造来源或标签保护。"""
import re


def normalize_environment(environ):
    """保留 GitLab 调用；GitHub 来源始终由原生字段覆盖继承的旧 CI 配置。"""
    environment = dict(environ)
    if environment.get("GITHUB_ACTIONS") != "true":
        return environment

    def require(condition, message):
        if not condition:
            raise ValueError(message)

    event = environment.get("GITHUB_EVENT_NAME")
    require(event in ("push", "workflow_dispatch"), "GitHub 仅允许 push 或手动受控运行")
    require(environment.get("GITHUB_SERVER_URL") == "https://github.com", "GitHub 来源服务地址不合法")
    repository = environment.get("GITHUB_REPOSITORY", "")
    require(re.fullmatch(r"[A-Za-z0-9][A-Za-z0-9-]*/[A-Za-z0-9_.-]+", repository)
            and repository.split("/")[-1] not in (".", ".."), "GitHub 来源仓库不合法")
    for field in ("GITHUB_RUN_ID", "GITHUB_RUN_NUMBER", "GITHUB_RUN_ATTEMPT"):
        require(re.fullmatch(r"[1-9][0-9]{0,19}", environment.get(field, "")), "GitHub 运行或重跑编号不合法")
    require(re.fullmatch(r"[a-f0-9]{40}", environment.get("GITHUB_SHA", "")), "GitHub 来源提交不合法")
    protected = environment.get("GITHUB_REF_PROTECTED")
    require(protected in ("true", "false"), "缺少 GitHub 真实引用保护状态")
    ref_type = environment.get("GITHUB_REF_TYPE")
    ref_name = environment.get("GITHUB_REF_NAME", "")
    require(ref_type in ("branch", "tag") and ref_name and not any(c.isspace() for c in ref_name),
            "GitHub 来源引用类型不合法")
    prefix = "refs/heads/" if ref_type == "branch" else "refs/tags/"
    require(environment.get("GITHUB_REF") == prefix + ref_name, "GitHub 完整引用与引用名称不一致")
    phases = {"build": 1, "test": 2, "package": 3, "publish": 4}
    arches = {"x64": 1, "arm64": 2}
    phase, arch = environment.get("LOWER_JOB_PHASE"), environment.get("LOWER_JOB_ARCH")
    require(phase in phases and arch in arches, "缺少受控 GitHub 阶段与架构")

    for field in ("CI_COMMIT_BRANCH", "CI_COMMIT_TAG", "CI_SERVER_URL", "CI_JOB_TOKEN"):
        environment.pop(field, None)
    # Cantor 配对唯一编码运行与重跑；低两位保留阶段和架构，不依赖作业名散列。
    run_id, attempt = int(environment["GITHUB_RUN_ID"]), int(environment["GITHUB_RUN_ATTEMPT"])
    pair = (run_id + attempt) * (run_id + attempt + 1) // 2 + attempt
    job_id = pair * 100 + phases[phase] * 10 + arches[arch]
    environment.update(CI_PIPELINE_SOURCE="push" if event == "push" else "web",
                       CI_PIPELINE_ID=environment["GITHUB_RUN_ID"],
                       CI_PIPELINE_IID=environment["GITHUB_RUN_NUMBER"], CI_JOB_ID=str(job_id),
                       CI_COMMIT_SHA=environment["GITHUB_SHA"],
                       CI_PROJECT_URL="https://github.com/" + repository,
                       CI_COMMIT_REF_PROTECTED=protected)
    environment["CI_COMMIT_BRANCH" if ref_type == "branch" else "CI_COMMIT_TAG"] = ref_name
    return environment
