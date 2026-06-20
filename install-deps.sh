#!/usr/bin/env sh
set -eu

sudo apt-get update
sudo apt-get install -y \
  build-essential \
  pkg-config \
  libgtk-3-dev \
  libsoapysdr-dev \
  soapysdr-tools \
  soapysdr-module-rtlsdr \
  soapysdr-module-hackrf \
  soapysdr-module-mirisdr
