# Standalone firmware entry. The application is components/rbgrid_app (also used by hosted builds such as ESPHome).
idf_component_register(SRCS "main.c"
                       INCLUDE_DIRS "."
                       REQUIRES rbgrid_app)
