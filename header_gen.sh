#!/bin/bash
wayland-scanner client-header /usr/share/wayland-protocols/staging/security-context/security-context-v1.xml security-context-v1-client-protocol.h
wayland-scanner private-code /usr/share/wayland-protocols/staging/security-context/security-context-v1.xml security-context-v1-protocol.c
