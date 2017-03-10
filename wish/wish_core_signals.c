#include "wish_core_signals.h"

void wish_core_signals(rpc_server_req* req, uint8_t* args) {
    wish_core_t* core = (wish_core_t*) req->server->context;

    int buffer_len = 300;
    uint8_t buffer[buffer_len];
    
    bson bs;
    bson_init_buffer(&bs, buffer, buffer_len);
    bson_append_start_array(&bs, "data");
    bson_append_string(&bs, "0", "ok");
    bson_append_finish_array(&bs);
    bson_finish(&bs);

    wish_rpc_server_emit(req, bson_data(&bs), bson_size(&bs));
    //wish_core_signals_emit_string(core, "ok");
}

void wish_core_signals_emit(wish_core_t* core, bson* signal) {
    wish_rpc_server_emit_broadcast(core->core_app_rpc_server, "signals", bson_data(signal), bson_size(signal));
}

void wish_core_signals_emit_string(wish_core_t* core, char* string) {
    int buf_len = 1024;
    char buf[1024];

    bson bs;
    bson_init_buffer(&bs, buf, buf_len);
    bson_append_start_array(&bs, "data");
    bson_append_string(&bs, "0", string);
    bson_append_finish_array(&bs);
    bson_finish(&bs);
    
    wish_rpc_server_emit_broadcast(core->core_app_rpc_server, "signals", bson_data(&bs), bson_size(&bs));
}

