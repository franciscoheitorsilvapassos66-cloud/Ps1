cmake_minimum_required(VERSION 3.21)
project(Snake3D LANGUAGES C ASM VERSION 1.0.0 DESCRIPTION "Snake 3D PS1")
psn00bsdk_add_executable(snake3d STATIC src/main.c)
psn00bsdk_add_cd_image(iso snake3d iso.xml DEPENDS snake3d system.cnf)
