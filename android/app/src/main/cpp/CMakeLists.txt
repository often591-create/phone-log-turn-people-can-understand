# 只列出需要加的部分：在 Android Studio「Native C++」範本的 CMakeLists.txt 裡，
# add_library 那一行加上 phonecheck.c（phonecheck.c 放在這個 cpp 資料夾）
add_library(${CMAKE_PROJECT_NAME} SHARED
        native-lib.cpp
        phonecheck.c)
