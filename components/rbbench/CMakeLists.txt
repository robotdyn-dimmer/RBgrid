# rbbench — internal development/test-rig debug console. Not part of the public source: this public tree
# carries an empty, header-only stub so `REQUIRES rbbench` resolves. Release builds never compile the console.
idf_component_register(INCLUDE_DIRS "include")
