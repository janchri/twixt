# Hello over twixt

Two MCUs, a (0x10) and b (0x11). Each serves an echo service of its own and calls the other's: all it takes to add a service.

```c
#define ECHO_SERVICE TWIXT_SERVICE_APP

static int echo(const struct twixt_req *req, uint8_t *rsp_data)
{
	rsp_data[0] = req->src;
	memcpy(&rsp_data[1], req->data, req->len);
	return 1 + req->len;
}

TWIXT_SERVICE_DEFINE(echo_service, ECHO_SERVICE, echo);
```

```c
twixt_request(peer, ECHO_SERVICE, msg, len, rsp, 1 + len);
```

## Build and run

```
twixt/scripts/twixt_run.sh twixt/samples/hello a b
```

```
[b] <inf> hello: 0x10 answers: protocol 1
[b] <inf> hello: 0x10 echoed "hello from mcu-b@11", from 0x11
[a] <inf> hello: 0x11 answers: protocol 1
[a] <inf> hello: 0x11 echoed "hello from mcu-a@10", from 0x10
```

Twister: `west twister -p native_sim -T twixt/samples/hello`
