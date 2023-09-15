/*
 * Copyright (c) 2023 Emcraft Systems
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdio.h>
#include <stdlib.h>
#include <errno.h>

#include <zephyr/net/socket.h>
#include <zephyr/kernel.h>
#include <zephyr/drivers/gpio.h>

#define BIND_PORT 4242

/*
 * Extract the LEDs and their GPIOs from the device tree
 */
#define DT_LEDS DT_COMPAT_GET_ANY_STATUS_OKAY(gpio_leds)
#define DT_LEDS_NUM ARRAY_SIZE(dt_led_name)
#define DT_NODE_GPIO(node_id) GPIO_DT_SPEC_GET(node_id, gpios)
static const char *dt_led_name[] = {
	DT_FOREACH_CHILD_SEP(DT_LEDS, DT_NODE_FULL_NAME, (,))
};
static const struct gpio_dt_spec dt_led_gpio[] = {
	DT_FOREACH_CHILD_SEP(DT_LEDS, DT_NODE_GPIO, (,))
};

static int get_request(int client, char **p)
{
	static char buf[128];
	int len;

	do {
		len = recv(client, buf, sizeof(buf) - 1, 0);
		if (len < 0) {
			printf("error: recv: %d\n", errno);
		}

		if (len <= 0) {
			return -1;
		}

		if (len >= 2 && buf[len - 2] == '\r' && buf[len - 1] == '\n') {
			len -= 2;
		}
	} while (len == 0);

	buf[len] = 0;
	*p = buf;

	printf("REQUEST: %s\n", buf);

	return 0;
}

static int send_response(int client, const char *p)
{
	int out_len;
	int len;
	static char eol[] = "\n";

	len = strlen(p);
	if (len) {
		printf("RESPONSE: %s\n", p);
	}

	while (len > 0) {
		out_len = send(client, p, len, 0);
		if (out_len < 0) {
			printf("error: send: %d\n", errno);
			return -1;
		}
		p += out_len;
		len -= out_len;
	}

	out_len = send(client, eol, sizeof(eol), 0);
	if (out_len < 0) {
		printf("error: send: %d\n", errno);
		return -1;
	}

	return 0;
}

static int usage(int client)
{
	const char *str[] = {
		"",
		"list: print supported LEDs",
		"<LED> on: turn on the specified LED",
		"<LED> off: turn off the specified LED",
		"",
	};

	for (int i = 0; i < ARRAY_SIZE(str); i++) {
		if (send_response(client, str[i]) < 0) {
			return -1;
		}
	}

	return 0;
}

int main(void)
{
	int serv;
	struct sockaddr_in bind_addr;
	int counter = 0;
	int ret;
	int i;

	for (i = 0; i < DT_LEDS_NUM; i++) {
		if (!gpio_is_ready_dt(&dt_led_gpio[i])) {
			printf("error: %s is not ready\n", dt_led_name[i]);
			exit(1);
		}

		ret = gpio_pin_configure_dt(&dt_led_gpio[i], GPIO_OUTPUT_INACTIVE);
		if (ret < 0) {
			printf("error: gpio_pin_configure_dt: %d\n", ret);
			exit(1);
		}

		printf("%s: ready\n", dt_led_name[i]);
	}

	serv = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);

	if (serv < 0) {
		printf("error: socket: %d\n", errno);
		exit(1);
	}

	bind_addr.sin_family = AF_INET;
	bind_addr.sin_addr.s_addr = htonl(INADDR_ANY);
	bind_addr.sin_port = htons(BIND_PORT);

	if (bind(serv, (struct sockaddr *)&bind_addr, sizeof(bind_addr)) < 0) {
		printf("error: bind: %d\n", errno);
		exit(1);
	}

	if (listen(serv, 5) < 0) {
		printf("error: listen: %d\n", errno);
		exit(1);
	}

	printf("Single-threaded TCP server waits for a connection\n"
	       "    on address %s port %d...\n",
	       CONFIG_NET_CONFIG_MY_IPV4_ADDR, BIND_PORT);

	while (1) {
		struct sockaddr_in client_addr;
		socklen_t client_addr_len = sizeof(client_addr);
		char addr_str[32];
		int client = accept(serv, (struct sockaddr *)&client_addr,
				    &client_addr_len);

		if (client < 0) {
			printf("error: accept: %d\n", errno);
			continue;
		}

		inet_ntop(client_addr.sin_family, &client_addr.sin_addr,
			  addr_str, sizeof(addr_str));
		printf("Connection #%d from %s\n", counter++, addr_str);

		if (usage(client) < 0) {
			goto error;
		}

		while (1) {
			char *request;

			ret = get_request(client, &request);
			if (ret < 0) {
				goto error;
			}

			if (strcmp(request, "list") == 0) {
				for (i = 0; i < DT_LEDS_NUM; i++) {
					ret = send_response(client, dt_led_name[i]);
					if (ret < 0) {
						goto error;
					}
				}
				ret = send_response(client, "");
				if (ret < 0) {
					goto error;
				}
				continue;
			}

			for (i = 0; i < DT_LEDS_NUM; i++) {
				int nlen = strlen(dt_led_name[i]);
				if (memcmp(request, dt_led_name[i], nlen) == 0) {
					int value = -1;
					if (strcmp(request + nlen, " off") == 0) {
						value = 0;
					} else if (strcmp(request + nlen, " on") == 0) {
						value = 1;
					}
					if (value >= 0) {
						ret = gpio_pin_set_dt(&dt_led_gpio[i], value);
						if (ret < 0) {
							printf("error: gpio_pin_set_dt: %d\n", ret);
							goto error;
						}
						ret = send_response(client, "OK");
						if (ret < 0) {
							goto error;
						}
						ret = send_response(client, "");
						if (ret < 0) {
							goto error;
						}
						break;
					}
				}
			}

			if (i == DT_LEDS_NUM) {
				if (usage(client) < 0) {
					goto error;
				}
			}
		}

error:
		close(client);
		printf("Connection from %s closed\n", addr_str);
	}
	return 0;
}
