#
# Regular cron jobs for the sni-proxy package
#
0 4	* * *	root	[ -x /usr/bin/sni-proxy_maintenance ] && /usr/bin/sni-proxy_maintenance
