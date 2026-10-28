# Copies the web editor bundle (web/dist/index.html.gz) to OUT, or the fallback page when it is missing.
# Runs at every build so that a fresh `npm run build` is picked up without reconfiguring.
if(EXISTS ${SRC})
  file(COPY_FILE ${SRC} ${OUT} ONLY_IF_DIFFERENT)
else()
  message(WARNING "web/dist/index.html.gz not found: run `npm run build` in web/ to embed the editor")
  file(COPY_FILE ${FALLBACK} ${OUT} ONLY_IF_DIFFERENT)
endif()
