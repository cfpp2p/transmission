--- J:/C/TurboTcompile1079FIXed072013/5424-FIX-072013/utils.c.original.c	Wed Jan 30 20:36:44 2013
+++ J:/C/TurboTcompile1079FIXed072013/5424-FIX-072013/utils.c	Sat Jul 20 20:17:34 2013
@@ -1153,6 +1153,7 @@
     int          err;
     int          port = 0;
     int          n;
+    bool         udp_will_fail = false;
     char *       tmp;
     char *       pch;
     const char * protocol = NULL;
@@ -1168,6 +1169,34 @@
 /*fprintf( stderr, "protocol is [%s]... what's left is [%s]\n", protocol, pch);*/
         if( ( n = strcspn( pch, ":/" ) ) )
         {
+            int       max_size_host_sub = 0;
+            int       host_sub_start = 0;
+            int       i;
+
+            // Ticket 5424 & 5426
+            err = !protocol;
+            if( !err )
+            {
+               if( !strcmp( protocol, "udp" ) ) {
+                  if( n < 256 ) {
+                  for( i=0; i<n; ++i ) {
+                  // find next dot
+                    if( pch[i] == 0x2E ) {
+                        max_size_host_sub = MAX( max_size_host_sub, (i - host_sub_start));
+                        host_sub_start = (i + 1);
+                    }
+                    if( max_size_host_sub > 63 ) break;
+                  }
+               if( i == n ) {
+                 --i;
+                 if( pch[i] != 0x2E ) max_size_host_sub = MAX( max_size_host_sub, (n - host_sub_start) );
+               }
+              if( max_size_host_sub > 63 ) udp_will_fail = true;
+              }
+              else udp_will_fail = true;
+             }
+            } // End Ticket 5424 & 5426
+
             const int havePort = pch[n] == ':';
             host = pch;
             pch += n;
@@ -1185,7 +1214,8 @@
         }
     }
 
-    err = !host || !path || !protocol;
+    err = !host || !path || !protocol || udp_will_fail;
+                                        // Ticket 5424 & 5426
 
     if( !err && !port )
     { 
