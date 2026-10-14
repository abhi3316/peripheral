#!/usr/bin/env bash
# ==============================================================================
# Peripheral Pan-Tilt Gateway & Operator Viewer - Complete Stack Runner
# ==============================================================================
# This script orchestrates and runs the full stack:
#  1. Builds the C++26 Peripheral Gateway Server (`peripheral_server`)
#  2. Builds and checks the Operator Viewer dependencies (`utils/viewer`)
#  3. Starts the official dual-camera simulator (`utils/tools`)
#  4. Launches the Peripheral Gateway Server (port 8080)
#  5. Launches the React Operator Viewer (port 5173)
#  6. Handles graceful cleanup on Ctrl+C (SIGINT/SIGTERM)
# ==============================================================================

set -e

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$ROOT_DIR"

# Color constants
BOLD="\033[1m"
GREEN="\033[32m"
CYAN="\033[36m"
YELLOW="\033[33m"
RED="\033[31m"
RESET="\033[0m"

echo -e "${BOLD}${CYAN}====================================================${RESET}"
echo -e "${BOLD}${CYAN}   Peripheral Gateway & Operator Viewer Runner     ${RESET}"
echo -e "${BOLD}${CYAN}====================================================${RESET}"

# PIDs of spawned background services
SIM_PID=""
STORAGE_PID=""
SERVER_PID=""
VIEWER_PID=""

cleanup() {
    echo ""
    echo -e "${YELLOW}[Runner] Stopping all services...${RESET}"
    
    if [ -n "$SERVER_PID" ] && kill -0 "$SERVER_PID" 2>/dev/null; then
        echo -e "${YELLOW}  Stopping Peripheral Server (PID: $SERVER_PID)...${RESET}"
        kill -TERM "$SERVER_PID" 2>/dev/null || true
    fi
    
    if [ -n "$VIEWER_PID" ] && kill -0 "$VIEWER_PID" 2>/dev/null; then
        echo -e "${YELLOW}  Stopping Operator Viewer (PID: $VIEWER_PID)...${RESET}"
        kill -TERM "$VIEWER_PID" 2>/dev/null || true
    fi

    if [ -n "$STORAGE_PID" ] && kill -0 "$STORAGE_PID" 2>/dev/null; then
        echo -e "${YELLOW}  Stopping Cloud Storage Simulator (PID: $STORAGE_PID)...${RESET}"
        kill -TERM "$STORAGE_PID" 2>/dev/null || true
    fi

    if [ -n "$SIM_PID" ] && kill -0 "$SIM_PID" 2>/dev/null; then
        echo -e "${YELLOW}  Stopping Camera Simulator (PID: $SIM_PID)...${RESET}"
        kill -TERM "$SIM_PID" 2>/dev/null || true
    fi

    # Wait briefly for graceful shutdown
    sleep 0.5
    echo -e "${GREEN}[Runner] All services stopped cleanly.${RESET}"
}

trap cleanup EXIT INT TERM

# ------------------------------------------------------------------------------
# 1. Build C++ Gateway Server
# ------------------------------------------------------------------------------
echo -e "${CYAN}[1/4] Building Peripheral Gateway Server (C++26)...${RESET}"
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --target peripheral_server -j

# ------------------------------------------------------------------------------
# 2. Build Operator Viewer Frontend
# ------------------------------------------------------------------------------
echo -e "${CYAN}[2/4] Ensuring Viewer dependencies & production bundle...${RESET}"
if [ ! -d "utils/viewer/node_modules" ]; then
    echo "  Installing viewer node_modules..."
    (cd utils/viewer && npm install)
fi
(cd utils/viewer && npm run build > /dev/null 2>&1)
echo -e "${GREEN}  Viewer bundle ready.${RESET}"

# ------------------------------------------------------------------------------
# 3. Start Official Camera Simulator & Cloud Storage (if not already running)
# ------------------------------------------------------------------------------
echo -e "${CYAN}[3/5] Starting Official Camera Simulator & Cloud Storage...${RESET}"
if lsof -i :7001 >/dev/null 2>&1 || lsof -i :7002 >/dev/null 2>&1; then
    echo -e "${YELLOW}  Camera simulator already active on ports 7001/7002.${RESET}"
else
    if [ ! -d "utils/tools/node_modules" ]; then
        (cd utils/tools && npm install)
    fi
    (cd utils/tools && npm run camera) &
    SIM_PID=$!
    
    # Wait for simulator to accept connections
    echo -n "  Waiting for camera simulator..."
    for i in {1..30}; do
        if lsof -i :7001 >/dev/null 2>&1; then
            echo -e " ${GREEN}connected!${RESET}"
            break
        fi
        sleep 0.2
        echo -n "."
    done
fi

if lsof -i :7100 >/dev/null 2>&1; then
    echo -e "${YELLOW}  Cloud storage simulator already active on port 7100.${RESET}"
else
    (cd utils/tools && npm run storage) &
    STORAGE_PID=$!
    
    echo -n "  Waiting for cloud storage on :7100..."
    for i in {1..30}; do
        if lsof -i :7100 >/dev/null 2>&1; then
            echo -e " ${GREEN}connected!${RESET}"
            break
        fi
        sleep 0.2
        echo -n "."
    done
fi

# ------------------------------------------------------------------------------
# 4. Start Peripheral Gateway Server
# ------------------------------------------------------------------------------
echo -e "${CYAN}[4/5] Starting Peripheral Gateway Server...${RESET}"
if lsof -i :8080 >/dev/null 2>&1; then
    echo -e "${YELLOW}  Port 8080 in use, stopping existing process...${RESET}"
    kill -9 $(lsof -t -i :8080) 2>/dev/null || true
    sleep 0.5
fi

./build/peripheral_server "$@" &
SERVER_PID=$!

echo -n "  Waiting for gateway server on :8080..."
for i in {1..30}; do
    if curl -s http://127.0.0.1:8080/api/state >/dev/null 2>&1; then
        echo -e " ${GREEN}online!${RESET}"
        break
    fi
    sleep 0.2
    echo -n "."
done

# ------------------------------------------------------------------------------
# 5. Start Operator Viewer Dev Server
# ------------------------------------------------------------------------------
if lsof -i :5173 >/dev/null 2>&1; then
    echo -e "${YELLOW}  Viewer dev server already running on :5173.${RESET}"
else
    (cd utils/viewer && npm run dev) >/dev/null 2>&1 &
    VIEWER_PID=$!
    
    echo -n "  Waiting for viewer dev server on :5173..."
    for i in {1..30}; do
        if lsof -i :5173 >/dev/null 2>&1; then
            echo -e " ${GREEN}ready!${RESET}"
            break
        fi
        sleep 0.2
        echo -n "."
    done
fi

# ------------------------------------------------------------------------------
# Dashboard Readout
# ------------------------------------------------------------------------------
echo ""
echo -e "${BOLD}${GREEN}================================================================${RESET}"
echo -e "${BOLD}${GREEN}  Peripheral Pan-Tilt Gateway Stack is Running!                 ${RESET}"
echo -e "${BOLD}${GREEN}================================================================${RESET}"
echo -e "  ${BOLD}Operator Viewer (Dev):${RESET}      ${CYAN}http://localhost:5173/${RESET}"
echo -e "  ${BOLD}Operator Viewer (Prod):${RESET}     ${CYAN}http://localhost:8080/${RESET}"
echo -e "  ${BOLD}Gateway State API:${RESET}          ${CYAN}http://localhost:8080/api/state${RESET}"
echo -e "  ${BOLD}Camera 0 Simulator Admin:${RESET}   ${CYAN}http://127.0.0.1:7000/admin/stats${RESET}"
echo -e "  ${BOLD}Camera 1 Simulator Admin:${RESET}   ${CYAN}http://127.0.0.1:7010/admin/stats${RESET}"
echo -e "  ${BOLD}Cloud Storage Admin:${RESET}        ${CYAN}http://127.0.0.1:7101/admin/objects${RESET}"
echo -e "  ${BOLD}Recordings Output Directory:${RESET}${CYAN}$ROOT_DIR/record/${RESET}"
echo -e "${BOLD}${GREEN}================================================================${RESET}"
echo -e "${YELLOW}Press Ctrl+C to terminate all services cleanly.${RESET}"
echo ""

# Keep running and wait for background jobs
wait "$SERVER_PID" 2>/dev/null || true
