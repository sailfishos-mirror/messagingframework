add_library(smime MODULE
  ../common/qgpgme.h ../common/qgpgme.cpp
  smimeplugin.h smimeplugin.cpp)
target_include_directories(smime
  PRIVATE ../common ../../../libraries/qmfclient ../../../libraries/qmfclient/support)
target_link_libraries(smime
  PRIVATE Qt6::Core QmfClient ${GPGME_LIBS})

install(TARGETS smime
  LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}/qt5/plugins/crypto)
