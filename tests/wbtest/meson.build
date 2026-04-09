wbtest = executable(
	'wbtest',
	files('bo.c', 'wbtest.c'),
	c_args : [libdrm_c_args, '-Wno-pointer-arith'],
	include_directories : [inc_root, inc_tests, inc_drm],
	dependencies : [dep_threads],
	link_with : [libdrm, libutil],
	install : false,
	link_args : ['-static'],
)

