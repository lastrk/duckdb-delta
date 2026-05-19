#!/usr/bin/env bash
# Idempotent rootless MinIO bring-up for the delta extension's S3 write-path tests.
#
# Usage:
#   scripts/local_minio.sh ensure         install + start + buckets (default)
#   scripts/local_minio.sh env            print export lines for AWS_* + DUCKDB_MINIO_TEST_SERVER_AVAILABLE
#   scripts/local_minio.sh status         show pid, endpoint, bucket list
#   scripts/local_minio.sh stop           stop the background MinIO process
#   scripts/local_minio.sh clean          stop + rm -rf data dir
#   scripts/local_minio.sh reset          wipe writable test prefixes (test-bucket/delta_writes)
#   scripts/local_minio.sh run <cmd...>   ensure + reset writable prefixes, then exec <cmd> with AWS_* + endpoint exported
#
# Tunables (env):
#   MINIO_DATA_DIR  (default: /tmp/minio-data)
#   MINIO_PORT      (default: 9000)

set -euo pipefail

MINIO_BIN="${HOME}/.local/bin/minio"
MC_BIN="${HOME}/.local/bin/mc"
DATA_DIR="${MINIO_DATA_DIR:-/tmp/minio-data}"
PID_FILE="${DATA_DIR}/minio.pid"
LOG_FILE="${DATA_DIR}/minio.log"
PORT="${MINIO_PORT:-9000}"
ENDPOINT="http://127.0.0.1:${PORT}"

ARCH="$(uname -m)"
case "$ARCH" in
	aarch64|arm64) DL_ARCH=linux-arm64 ;;
	x86_64)        DL_ARCH=linux-amd64 ;;
	*) echo "[error] unsupported arch: $ARCH" >&2; exit 2 ;;
esac

# AWS_* values must match scripts/env_minio so existing tests/secrets see the same creds.
# Note: AWS_ENDPOINT includes the scheme (for AWS-SDK style usage); DuckDB's S3
# secret ENDPOINT expects the scheme-less host:port form — exposed as DUCKDB_S3_ENDPOINT.
export AWS_ACCESS_KEY_ID=minio_duckdb_user
export AWS_SECRET_ACCESS_KEY=minio_duckdb_user_password
export AWS_DEFAULT_REGION=eu-west-1
export AWS_ENDPOINT="${ENDPOINT}"
export DUCKDB_S3_ENDPOINT="127.0.0.1:${PORT}"
export DUCKDB_MINIO_TEST_SERVER_AVAILABLE=1
export S3_TEST_SERVER_AVAILABLE=1

need_install_minio() { [[ ! -x "$MINIO_BIN" ]]; }
need_install_mc()    { [[ ! -x "$MC_BIN" ]]; }
minio_running() {
	[[ -f "$PID_FILE" ]] && kill -0 "$(cat "$PID_FILE")" 2>/dev/null
}

install_minio() {
	if ! need_install_minio; then
		echo "[skip] minio already installed at $MINIO_BIN"
		return
	fi
	mkdir -p "$(dirname "$MINIO_BIN")"
	echo "[install] downloading minio ($DL_ARCH) -> $MINIO_BIN"
	curl -fL --progress-bar -o "$MINIO_BIN" "https://dl.min.io/server/minio/release/${DL_ARCH}/minio"
	chmod +x "$MINIO_BIN"
}

install_mc() {
	if ! need_install_mc; then
		echo "[skip] mc already installed at $MC_BIN"
		return
	fi
	mkdir -p "$(dirname "$MC_BIN")"
	echo "[install] downloading mc ($DL_ARCH) -> $MC_BIN"
	curl -fL --progress-bar -o "$MC_BIN" "https://dl.min.io/client/mc/release/${DL_ARCH}/mc"
	chmod +x "$MC_BIN"
}

start_minio() {
	if minio_running; then
		echo "[skip] minio already running (pid $(cat "$PID_FILE"), $ENDPOINT)"
		return
	fi
	mkdir -p "$DATA_DIR"
	echo "[start] minio server $DATA_DIR --address 127.0.0.1:$PORT"
	MINIO_ROOT_USER="$AWS_ACCESS_KEY_ID" \
	MINIO_ROOT_PASSWORD="$AWS_SECRET_ACCESS_KEY" \
		nohup "$MINIO_BIN" server "$DATA_DIR" --address "127.0.0.1:$PORT" \
		> "$LOG_FILE" 2>&1 &
	echo $! > "$PID_FILE"
	# Wait for readiness: poll the health endpoint up to ~15s.
	for _ in $(seq 1 30); do
		if curl -fsS "$ENDPOINT/minio/health/ready" > /dev/null 2>&1; then
			echo "[ready] $ENDPOINT"
			return
		fi
		sleep 0.5
	done
	echo "[error] minio did not become ready; see $LOG_FILE" >&2
	exit 1
}

ensure_buckets() {
	"$MC_BIN" alias set local "$ENDPOINT" "$AWS_ACCESS_KEY_ID" "$AWS_SECRET_ACCESS_KEY" >/dev/null
	for bucket in test-bucket test-bucket-public; do
		if "$MC_BIN" ls "local/$bucket" >/dev/null 2>&1; then
			echo "[skip] bucket $bucket exists"
		else
			echo "[create] bucket $bucket"
			"$MC_BIN" mb -p "local/$bucket" >/dev/null
		fi
	done
	# Public-read on the public bucket (idempotent).
	"$MC_BIN" anonymous set download local/test-bucket-public >/dev/null
}

# Wipe writable test prefixes so write-side tests start from a clean slate.
# Read fixtures (test-bucket/dat, test-bucket-public/dat) are preserved.
reset_write_prefixes() {
	"$MC_BIN" alias set local "$ENDPOINT" "$AWS_ACCESS_KEY_ID" "$AWS_SECRET_ACCESS_KEY" >/dev/null
	for prefix in test-bucket/delta_writes; do
		if "$MC_BIN" ls "local/$prefix" >/dev/null 2>&1; then
			"$MC_BIN" rm --recursive --force "local/$prefix" >/dev/null 2>&1 || true
			echo "[reset] wiped local/$prefix"
		fi
	done
}

case "${1:-ensure}" in
	ensure)
		install_minio
		install_mc
		start_minio
		ensure_buckets
		;;
	env)
		cat <<EOF
export AWS_ACCESS_KEY_ID=$AWS_ACCESS_KEY_ID
export AWS_SECRET_ACCESS_KEY=$AWS_SECRET_ACCESS_KEY
export AWS_DEFAULT_REGION=$AWS_DEFAULT_REGION
export AWS_ENDPOINT=$AWS_ENDPOINT
export DUCKDB_S3_ENDPOINT=$DUCKDB_S3_ENDPOINT
export DUCKDB_MINIO_TEST_SERVER_AVAILABLE=$DUCKDB_MINIO_TEST_SERVER_AVAILABLE
export S3_TEST_SERVER_AVAILABLE=$S3_TEST_SERVER_AVAILABLE
EOF
		;;
	status)
		if minio_running; then
			echo "running pid=$(cat "$PID_FILE") endpoint=$ENDPOINT data=$DATA_DIR"
			if [[ -x "$MC_BIN" ]]; then
				"$MC_BIN" ls local/ 2>/dev/null | sed 's/^/  /'
			fi
		else
			echo "not running"
		fi
		;;
	stop)
		if minio_running; then
			kill "$(cat "$PID_FILE")" && echo "[stop] minio stopped"
			rm -f "$PID_FILE"
		else
			echo "[skip] not running"
		fi
		;;
	clean)
		"$0" stop || true
		rm -rf "$DATA_DIR"
		echo "[clean] removed $DATA_DIR"
		;;
	reset)
		install_minio >/dev/null
		install_mc    >/dev/null
		start_minio   >/dev/null
		reset_write_prefixes
		;;
	run)
		shift
		"$0" ensure >/dev/null
		reset_write_prefixes >/dev/null
		exec env \
			AWS_ACCESS_KEY_ID="$AWS_ACCESS_KEY_ID" \
			AWS_SECRET_ACCESS_KEY="$AWS_SECRET_ACCESS_KEY" \
			AWS_DEFAULT_REGION="$AWS_DEFAULT_REGION" \
			AWS_ENDPOINT="$AWS_ENDPOINT" \
			DUCKDB_S3_ENDPOINT="$DUCKDB_S3_ENDPOINT" \
			DUCKDB_MINIO_TEST_SERVER_AVAILABLE="$DUCKDB_MINIO_TEST_SERVER_AVAILABLE" \
			S3_TEST_SERVER_AVAILABLE="$S3_TEST_SERVER_AVAILABLE" \
			"$@"
		;;
	*)
		echo "unknown subcommand: ${1:-}" >&2
		echo "usage: $0 {ensure|env|status|stop|clean|run <cmd...>}" >&2
		exit 2
		;;
esac
