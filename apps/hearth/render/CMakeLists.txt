# ac3hearth-render (planning/ac4.md, I2): one item through Hearth's engine - the player, session and
# stream decoder the window plays it with - into a WAV file, so tools/checks/gain_ac4_decode.py
# --engine holds what the engine puts out to the formulas it holds ac3cli decode's to. Qt-free; never
# installed or packaged.
add_executable(ac3hearth-render main.cpp)
target_compile_features(ac3hearth-render PRIVATE cxx_std_23)
target_link_libraries(ac3hearth-render PRIVATE ac3hearth_engine ac3::warnings)
