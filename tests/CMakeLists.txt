add_executable(sa_tests
    TestMain.cpp
    TestJson.cpp
    TestLockFree.cpp
    TestDsp.cpp
    TestSignatureScanner.cpp
    TestBspGeometry.cpp
    TestLzma.cpp
    TestGmaArchive.cpp
    TestEngineFileSystem.cpp
    TestStaticProps.cpp
    TestSurfaceProps.cpp
    TestSoundOverrides.cpp
    TestDynamicOccluders.cpp
    TestRoomDsp.cpp
    TestAutoDirectivity.cpp
    TestChannelCapture.cpp
    TestAudioThreadClock.cpp
    TestHrtfRenderer.cpp
    TestPathCache.cpp
    TestPagedPathingSDK.cpp
    TestOcclusionShaping.cpp
)
target_link_libraries(sa_tests PRIVATE sa_core)
target_include_directories(sa_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME sa_tests COMMAND sa_tests)
