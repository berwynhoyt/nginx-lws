/*
 * LWS state
 *
 * Copyright (C) 2023 Andre Naef
 */


#include <lws_state.h>
#include <lualib.h>
#include <lauxlib.h>
#include <lws_lib.h>
#include <lws_profiler.h>


static void *lws_alloc_unchecked(void *ud, void *ptr, size_t osize, size_t nsize);
static void *lws_alloc_checked(void *ud, void *ptr, size_t osize, size_t nsize);
static void lws_set_path(lua_State *L, int index, const char *field);
static int lws_init(lua_State *L);
static void lws_set_state_timer(lws_state_t *state);
static void lws_state_timer_handler(ngx_event_t *ev);
static lws_state_t *lws_create_state(lws_request_ctx_t *ctx);


static void *lws_alloc_unchecked (void *ud, void *ptr, size_t osize, size_t nsize) {
	if (nsize == 0) {
		free(ptr);
		return NULL;
	}
	return realloc(ptr, nsize);
}

static void *lws_alloc_checked (void *ud, void *ptr, size_t osize, size_t nsize) {
	size_t        memory_used;
	lws_state_t  *state;

	state = ud;
	if (nsize == 0) {
		free(ptr);
		state->memory_used -= osize;
		return NULL;
	}
	memory_used = state->memory_used - osize + nsize;
	if (memory_used > state->memory_max) {
		return NULL;
	}
	ptr = realloc(ptr, nsize);
	if (ptr) {
		state->memory_used = memory_used;
	}
	return ptr;
}

static void lws_set_path (lua_State *L, int index, const char *field) {
	size_t       path_len;
	const char  *path;

	/* check path */
	path = lua_tolstring(L, index, &path_len);
	if (path_len == 0) {
		return;
	}

	/* get loader */
	if (lua_getglobal(L, LUA_LOADLIBNAME) != LUA_TTABLE) {
		luaL_error(L, "failed to get loader");
	}

	/* process path */
	if (path[0] == '+') {
		if (lua_getfield(L, -1, field) != LUA_TSTRING) {
			luaL_error(L, "failed to get loader %s", field);
		}
		lua_pushliteral(L, ";");
		lua_pushlstring(L, ++path, --path_len);
		lua_concat(L, 3);
	} else {
		lua_pushvalue(L, index);
	}
        lua_setfield(L, -2, field);
        lua_pop(L, 1);
}

static int lws_init (lua_State *L) {
	/* open standard libraries */
	luaL_openlibs(L);

	/* open LWS library */
	luaL_requiref(L, LWS_LIB_NAME, lws_open_lws, 1);

	/* set paths */
	lws_set_path(L, 1, "path");
	lws_set_path(L, 2, "cpath");

	/* open profiler */
	if (lua_toboolean(L, 3)) {
		lua_pushcfunction(L, lws_open_profiler);
		lua_call(L, 0, 0);
	}

	return 0;
}

static void lws_set_state_timer (lws_state_t *state) {
	ngx_msec_t  next;

	if (state->tev.timer_set) {
		ngx_del_timer(&state->tev);
	}
	if (state->time_max != NGX_TIMER_INFINITE || state->timeout != NGX_TIMER_INFINITE) {
		next = state->timeout < state->time_max ? state->timeout : state->time_max;
		ngx_add_timer(&state->tev, next - ngx_current_msec);
	}
}

static void lws_state_timer_handler (ngx_event_t *ev) {
	lws_state_t  *state;

	state = ev->data;
	if (!state->in_use) {
		ngx_queue_remove(&state->queue);
		lws_close_state(state, ev->log);
	} else {
		/* handled when request completes; setting state->close could be race condition */
	}
}

static lws_state_t *lws_create_state (lws_request_ctx_t *ctx) {
	ngx_log_t        *log;
	ngx_str_t         msg;
	lws_state_t      *state;
	lws_loc_conf_t   *llcf;
	lws_main_conf_t  *lmcf;

	/* create state */
	log = ctx->r->connection->log;
	state = ngx_calloc(sizeof(lws_state_t), log);
	if (!state) {
		ngx_log_error(NGX_LOG_CRIT, log, 0, "[LWS] failed to allocate state");
		return NULL;
	}
	llcf = ngx_http_get_module_loc_conf(ctx->r, lws_module);
	state->llcf = llcf;

	/* create Lua state */
	if (llcf->state_memory_max > 0) {
		state->memory_max = llcf->state_memory_max;
		state->L = lua_newstate(lws_alloc_checked, state);
	} else {
		state->L = lua_newstate(lws_alloc_unchecked, NULL);
	}
	if (!state->L) {
		ngx_log_error(NGX_LOG_CRIT, log, 0, "[LWS] failed to create Lua state");
		return NULL;
	}

	/* initialize Lua state */
	lmcf = ngx_http_get_module_main_conf(ctx->r, lws_module);
	lua_pushcfunction(state->L, lws_init);
	lua_pushlstring(state->L, (const char *)llcf->path.data, llcf->path.len);
	lua_pushlstring(state->L, (const char *)llcf->cpath.data, llcf->cpath.len);
	lua_pushboolean(state->L, lmcf->monitor != NULL);
	if (lua_pcall(state->L, 3, 0, 0) != LUA_OK) {
		lws_get_msg(state->L, -1, &msg);
		ngx_log_error(NGX_LOG_CRIT, log, 0, "[LWS] failed to initialize Lua state: %V",
				&msg);
		lws_close_state(state, log);
		return NULL;
	}

	/* push traceback */
	lua_pushcfunction(state->L, lws_traceback);

	/* set timer */
	if (llcf->state_time_max) {
		state->time_max = ngx_current_msec + llcf->state_time_max;
	} else {
		state->time_max = NGX_TIMER_INFINITE;
	}
	state->timeout = NGX_TIMER_INFINITE;
	state->tev.data = state;
	state->tev.handler = lws_state_timer_handler;
	state->tev.log = ngx_cycle->log;
	lws_set_state_timer(state);

	/* done */
	llcf->states_n++;
	if (lmcf->monitor) {
		ngx_atomic_fetch_add(&lmcf->monitor->states_n, 1);
	}
	ngx_log_error(NGX_LOG_INFO, log, 0, "[LWS] %s state created L:%p", LUA_VERSION, state->L);
	return state;
}

void lws_close_state (lws_state_t *state, ngx_log_t *log) {
	lws_main_conf_t  *lmcf;

	lua_close(state->L);
	state->llcf->states_n--;
	lmcf = ngx_http_cycle_get_module_main_conf(ngx_cycle, lws_module);
	if (lmcf->monitor) {
		ngx_atomic_fetch_add(&lmcf->monitor->states_n, -1);
		ngx_atomic_fetch_add(&lmcf->monitor->memory_used, 0 - state->memory_monitor);
	}
	ngx_log_error(NGX_LOG_INFO, log, 0, "[LWS] %s state closed L:%p", LUA_VERSION, state->L);
	ngx_free(state);
}

int lws_acquire_state (lws_request_ctx_t *ctx) {
	lws_state_t      *state;
	ngx_queue_t      *q;
	lws_loc_conf_t   *llcf;
	lws_main_conf_t  *lmcf;

	llcf = ngx_http_get_module_loc_conf(ctx->r, lws_module);
	if (!ngx_queue_empty(&llcf->states)) {
		q = ngx_queue_head(&llcf->states);
		ngx_queue_remove(q);
		state = ngx_queue_data(q, lws_state_t, queue);
		if (llcf->state_timeout > 0) {
			state->timeout = NGX_TIMER_INFINITE;
			lws_set_state_timer(state);
		}
	} else {
		state = lws_create_state(ctx);
		if (!state) {
			return -1;
		}
	}
	lmcf = ngx_http_get_module_main_conf(ctx->r, lws_module);
	state->profiler = lmcf->monitor ? lmcf->monitor->profiler : 0;
	state->in_use = 1;
	ctx->state = state;
	return 0;
}

void lws_release_state (lws_request_ctx_t *ctx) {
	lws_state_t      *state;
	lws_loc_conf_t   *llcf;
	lws_main_conf_t  *lmcf;

	/* count request */
	state = ctx->state;
	state->request_count++;
	lmcf = ngx_http_get_module_main_conf(ctx->r, lws_module);
	if (lmcf->monitor) {
		ngx_atomic_fetch_add(&lmcf->monitor->request_count, 1);
	}

	/* close state? */
	llcf = state->llcf;
	if (state->close || state->tev.timedout || (llcf->state_requests_max > 0
			&& state->request_count >= llcf->state_requests_max)) {
		lws_close_state(state, ctx->r->connection->log);
		return;
	}

	/* perform GC and update monitor as needed */
	if (!llcf->state_memory_max && (llcf->state_gc > 0 || lmcf->monitor)) {
		/* update used memory from Lua state */
		state->memory_used = (size_t)lua_gc(state->L, LUA_GCCOUNT, 0) * 1024
				+ lua_gc(state->L, LUA_GCCOUNTB, 0);
	}
	if (llcf->state_gc > 0 && state->memory_used > llcf->state_gc) {
#ifdef NGX_DEBUG
		size_t  memory_used = state->memory_used;
#endif
		lua_gc(state->L, LUA_GCCOLLECT, 0);
		if (!llcf->state_memory_max) {
			state->memory_used = (size_t)lua_gc(state->L, LUA_GCCOUNT, 0) * 1024
					+ lua_gc(state->L, LUA_GCCOUNTB, 0);
		}
		ngx_log_debug3(NGX_LOG_DEBUG_HTTP, ctx->r->connection->log, 0,
				"[LWS] GC L:%p before:%z after:%z", state->L, memory_used,
				state->memory_used);
	}
	if (lmcf->monitor) {
		ngx_atomic_fetch_add(&lmcf->monitor->memory_used, state->memory_used
				- state->memory_monitor);
		state->memory_monitor = state->memory_used;
	}

	/* update timeout */
	if (llcf->state_timeout > 0) {
		state->timeout = ngx_current_msec + llcf->state_timeout;
		lws_set_state_timer(state);
	}

	/* done */
	state->in_use = 0;
	ngx_queue_insert_head(&llcf->states, &state->queue);
}

int lws_run_state (lws_request_ctx_t *ctx) {
	int         result;
	lua_State  *L;
	ngx_log_t  *log;
	ngx_str_t   msg;

	/* prepare stack */
	L = ctx->state->L;
	lua_pushcfunction(L, lws_run);
	lua_pushlightuserdata(L, ctx);  /* [traceback, function, ctx] */

	/* call */
	if (lua_pcall(L, 1, 1, 1) == LUA_OK) {
		result = lua_tointeger(L, -1);
	} else {
		/* set error result, mark for close */
		result = -1;
		ctx->state->close = 1;

		/* log error */
		log = ctx->r->connection->log;
		lws_get_msg(L, -1, &msg);
		ngx_log_error(NGX_LOG_ERR, log, 0, "[LWS] %s error: %V", LUA_VERSION, &msg);
		if (!ctx->state->llcf->diagnostic) {
			goto done;
		}

		/* store diagnostic */
		ctx->diagnostic.data = ngx_alloc(msg.len, log);
		if (!ctx->diagnostic.data) {
			ngx_log_error(NGX_LOG_ERR, log, 0, "[LWS] failed to allocate diagnostic");
			goto done;
		}
		ngx_memcpy(ctx->diagnostic.data, msg.data, msg.len);
		ctx->diagnostic.len = msg.len;
	}  /* [traceback, result] */

	/* clear result */
	done:
	lua_pop(L, 1);  /* [traceback] */

	return result;
}
