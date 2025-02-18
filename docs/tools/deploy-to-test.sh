#!/usr/bin/env bash
#
# README:
# This script deploys ClickHouse website to your personal test subdomain.
#
# Before first use of this script:
# 1) Set up building documentation according to https://github.com/ClickHouse/ClickHouse/tree/master/docs/tools#use-buildpy-use-build-py
# 2) Create https://github.com/GIT_USER/clickhouse.github.io repo (replace GIT_USER with your GitHub login)
# 3) Enable GitHub Pages in settings of this repo
# 4) Add file named CNAME in root of this repo with "GIT_USER-test.clickhouse.com" content (without quotes)
# 5) Send email on address from https://clickhouse.com/#contacts asking to create GIT_USER-test.clickhouse.com domain
#
set -ex

BASE_DIR=$(dirname "$(readlink -f "$0")")
GIT_USER=${GIT_USER:-$USER}

GIT_PROD_URI=git@github.com:${GIT_USER}/clickhouse.github.io.git \
 BASE_DOMAIN=${GIT_USER}-test.clickhouse.com \
 EXTRA_BUILD_ARGS="${*}" \
 CLOUDFLARE_TOKEN="" \
 "${BASE_DIR}/release.sh"
