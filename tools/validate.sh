#!/usr/bin/env bash
# 门禁薄封装:真正的实现在框架仓的 tools/validate.sh,应用仓只负责把
# 自己的仓库根传过去,避免每个应用各抄一份门禁逻辑。
set -euo pipefail
app_root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
exec "${app_root}/components/framework/tools/validate.sh" --project-root "${app_root}" "$@"
