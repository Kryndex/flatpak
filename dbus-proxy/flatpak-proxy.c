/*
 * Copyright © 2015 Red Hat, Inc
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.	 See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library. If not, see <http://www.gnu.org/licenses/>.
 *
 * Authors:
 *       Alexander Larsson <alexl@redhat.com>
 */

#include "config.h"

#include <unistd.h>
#include <string.h>

#include "flatpak-proxy.h"

#include <gio/gunixsocketaddress.h>
#include <gio/gunixconnection.h>
#include <gio/gunixfdmessage.h>

/**
 * The proxy listens to a unix domain socket, and for each new
 * connection it opens up a new connection to a specified dbus bus
 * address (typically the session bus) and forwards data between the
 * two. During the authentication phase all data is forwarded as
 * received, and additionally for the first 1 byte zero we also send
 * the proxy credentials to the bus.
 *
 * Once the connection is authenticated there are two modes, filtered
 * and unfiltered. In the unfiltered mode we just send all messages on
 * as we receive, but in the in the filtering mode we apply a policy,
 * which is similar to the policy supported by kdbus.
 *
 * The policy for the filtering consists of a mapping from well-known
 * names to a policy that is either SEE, TALK or OWN. The default
 * initial policy is that the the user is only allowed to TALK to the
 * bus itself (org.freedesktop.DBus, or no destination specified), and
 * TALK to its own unique id. All other clients are invisible. The
 * well-known names can be specified exactly, or as a simple one-level
 * wildcard like "org.foo.*" which matches "org.foo.bar", but not
 * "org.foobar" or "org.foo.bar.gazonk".
 *
 * Polices are specified for well-known names, but they also affect
 * the owner of that name, so that the policy for a unique id is the
 * superset of the polices for all the names it owns. Due to technical
 * reasons the policy for a unique name is "sticky", in that we keep
 * the highest policy granted by a once-owned name even when the client
 * releases that name. This is impossible to avoid in a race-free way
 * in a proxy. But this is rarely a problem in practice, as clients
 * rarely release names and stay on the bus.
 *
 * Here is a description of the policy levels:
 * (all policy levels also imply the ones before it)
 *
 * SEE:
 *    The name/id is visible in the ListNames reply
 *    The name/id is visible in the ListActivatableNames reply
 *    You can call GetNameOwner on the name
 *    You can call NameHasOwner on the name
 *    You see NameOwnerChanged signals on the name
 *    You see NameOwnerChanged signals on the id when the client disconnects
 *    You can call the GetXXX methods on the name/id to get e.g. the peer pid
 *    You get AccessDenied rather than NameHasNoOwner when sending messages to the name/id
 *
 * TALK:
 *    You can send method calls and signals to the name/id
 *    You will receive broadcast signals from the name/id (if you have a match rule for them)
 *    You can call StartServiceByName on the name
 *
 * OWN:
 *    You are allowed to call RequestName/ReleaseName/ListQueuedOwners on the name.
 *
 * The policy applies only to signals and method calls. All replies
 * (errors or method returns) are allowed once for an outstanding
 * method call, and never otherwise.
 *
 * Every peer on the bus is considered priviledged, and we thus trust
 * it. So we rely on similar proxies to be running for all untrusted
 * clients. Any such priviledged peer is allowed to send method call
 * or unicast signal messages to the proxied client. Once another peer
 * sends you a message the unique id of that peer is now made visible
 * (policy SEE) to the proxied client, allowing the client to track
 * caller lifetimes via NameOwnerChanged signals.
 *
 * Differences to kdbus custom endpoint policies:
 *
 *  * The proxy will return the credentials (like pid) of the proxy,
 *    not the real client.
 *
 *  * Policy is not dropped when a peer releases a name.
 *
 *  * Peers that call you become visible (SEE) (and get signals for
 *    NameOwnerChange disconnect) In kdbus currently custom endpoints
 *    never get NameOwnerChange signals for unique ids, but this is
 *    problematic as it disallows a services to track lifetimes of its
 *    clients.
 *
 * Mode of operation
 *
 * Once authenticated we receive incoming messages one at a time,
 * and then we demarshal the message headers to make routing decisions.
 * This means we trust the bus to do message format validation, etc.
 * (because we don't parse the body). Also we assume that the bus verifies
 * reply_serials, i.e. that a reply can only be sent once and by the real
 * recipient of an previously sent method call.
 *
 * We don't however trust the serials from the client. We verify that
 * they are strictly increasing to make sure the code is not confused
 * by serials being reused.
 *
 * In order to track the ownership of the allowed names we hijack the
 * connection after the initial Hello message, sending AddMatch,
 * ListNames and GetNameOwner messages to get a proper view of who
 * owns the names atm. Then we listen to NameOwnerChanged events for
 * further updates. This causes a slight offset between serials in the
 * client and serials as seen by the bus.
 *
 * After that the filter is strictly passive, in that we never
 * construct our own requests. For each message received from the
 * client we look up the type and the destination policy and make a
 * decision to either pass it on as is, rewrite it before passing on
 * (for instance ListName replies), drop it completely, or return a
 * made-up reply/error to the sender.
 *
 * When returning a made-up reply we replace the actual message with a
 * Ping request to the bus with the same serial and replace the resulting
 * reply with the made up reply (with the serial from the Ping reply).
 * This means we keep the strict message ordering and serial numbers of
 * the bus.
 *
 * Policy is applied to unique ids in the following cases:
 *  * During startup we call AddWatch for signals on all policy names
 *    and wildcards (using arg0namespace) so that we get NameOwnerChanged
 *    events which we use to update the unique id policies.
 *  * During startup we create synthetic GetNameOwner requests for all
 *    normal policy names, and if there are wildcarded names we create a
 *    synthetic ListNames request and use the results of that to do further
 *    GetNameOwner for the existing names matching the wildcards. When we get
 *    replies for the GetNameOwner requests the unique id policy is updated.
 *  * When we get a method call from a unique id, it gets SEE
 *  * When we get a reply to the initial Hello request we give
 *    our own assigned unique id policy TALK.
 *
 * All messages sent to the bus itself are fully demarshalled
 * and handled on a per-method basis:
 *
 * Hello, AddMatch, RemoveMatch, GetId: Always allowed
 * ListNames, ListActivatableNames: Always allowed, but response filtered
 * UpdateActivationEnvironment, BecomeMonitor: Always denied
 * RequestName, ReleaseName, ListQueuedOwners: Only allowed if arg0 is a name with policy OWN
 * NameHasOwner, GetNameOwner: Only pass on if arg0 is a name with policy SEE, otherwise return synthetic reply
 * StartServiceByName: Only allowed if policy TALK on arg0
 * GetConnectionUnixProcessID, GetConnectionCredentials,
 *  GetAdtAuditSessionData, GetConnectionSELinuxSecurityContext,
 *  GetConnectionUnixUser: Allowed if policy SEE on arg0
 *
 * For unknown methods, we return a synthetic error.
 */

typedef struct FlatpakProxyClient FlatpakProxyClient;

FlatpakPolicy flatpak_proxy_get_policy (FlatpakProxy *proxy,
                                        const char   *name);

/* We start looking ignoring the first cr-lf
   since there is no previous line initially */
#define AUTH_END_INIT_OFFSET 2
#define AUTH_END_STRING "\r\nBEGIN\r\n"

typedef enum {
  EXPECTED_REPLY_NONE,
  EXPECTED_REPLY_NORMAL,
  EXPECTED_REPLY_HELLO,
  EXPECTED_REPLY_FILTER,
  EXPECTED_REPLY_FAKE_GET_NAME_OWNER,
  EXPECTED_REPLY_FAKE_LIST_NAMES,
  EXPECTED_REPLY_LIST_NAMES,
  EXPECTED_REPLY_REWRITE,
} ExpectedReplyType;

typedef struct
{
  gsize    size;
  gsize    pos;
  int      refcount;
  gboolean send_credentials;
  GList   *control_messages;

  guchar   data[16];
  /* data continues here */
} Buffer;

typedef struct
{
  Buffer     *buffer;
  gboolean    big_endian;
  guchar      type;
  guchar      flags;
  guint32     length;
  guint32     serial;
  const char *path;
  const char *interface;
  const char *member;
  const char *error_name;
  const char *destination;
  const char *sender;
  const char *signature;
  gboolean    has_reply_serial;
  guint32     reply_serial;
  guint32     unix_fds;
} Header;

static void header_free (Header *header);
G_DEFINE_AUTOPTR_CLEANUP_FUNC (Header, header_free)

typedef struct
{
  gboolean            got_first_byte; /* always true on bus side */
  gboolean            closed; /* always true on bus side */

  FlatpakProxyClient *client;
  GSocketConnection  *connection;
  GSource            *in_source;
  GSource            *out_source;

  GBytes             *extra_input_data;
  Buffer             *current_read_buffer;
  Buffer              header_buffer;

  GList              *buffers; /* to be sent */
  GList              *control_messages;

  GHashTable         *expected_replies;
} ProxySide;

struct FlatpakProxyClient
{
  GObject       parent;

  FlatpakProxy *proxy;

  gboolean      authenticated;
  int           auth_end_offset;

  ProxySide     client_side;
  ProxySide     bus_side;

  /* Filtering data: */
  guint32     serial_offset;
  guint32     hello_serial;
  guint32     last_serial;
  GHashTable *rewrite_reply;
  GHashTable *get_owner_reply;

  GHashTable *unique_id_policy;
};

typedef struct
{
  GObjectClass parent_class;
} FlatpakProxyClientClass;

struct FlatpakProxy
{
  GSocketService parent;

  gboolean       log_messages;

  GList         *clients;
  char          *socket_path;
  char          *dbus_address;

  gboolean       filter;

  GHashTable    *wildcard_policy;
  GHashTable    *policy;
};

typedef struct
{
  GSocketServiceClass parent_class;
} FlatpakProxyClass;


enum {
  PROP_0,

  PROP_DBUS_ADDRESS,
  PROP_SOCKET_PATH
};

#define FLATPAK_TYPE_PROXY flatpak_proxy_get_type ()
#define FLATPAK_PROXY(obj) (G_TYPE_CHECK_INSTANCE_CAST ((obj), FLATPAK_TYPE_PROXY, FlatpakProxy))
#define FLATPAK_IS_PROXY(obj) (G_TYPE_CHECK_INSTANCE_TYPE ((obj), FLATPAK_TYPE_PROXY))


#define FLATPAK_TYPE_PROXY_CLIENT flatpak_proxy_client_get_type ()
#define FLATPAK_PROXY_CLIENT(obj) (G_TYPE_CHECK_INSTANCE_CAST ((obj), FLATPAK_TYPE_PROXY_CLIENT, FlatpakProxyClient))
#define FLATPAK_IS_PROXY_CLIENT(obj) (G_TYPE_CHECK_INSTANCE_TYPE ((obj), FLATPAK_TYPE_PROXY_CLIENT))

GType flatpak_proxy_client_get_type (void);

G_DEFINE_TYPE (FlatpakProxy, flatpak_proxy, G_TYPE_SOCKET_SERVICE)
G_DEFINE_TYPE (FlatpakProxyClient, flatpak_proxy_client, G_TYPE_OBJECT)

static void start_reading (ProxySide *side);
static void stop_reading (ProxySide *side);

static void
buffer_unref (Buffer *buffer)
{
  g_assert (buffer->refcount > 0);
  buffer->refcount--;

  if (buffer->refcount == 0)
    {
      g_list_free_full (buffer->control_messages, g_object_unref);
      g_free (buffer);
    }
}

static Buffer *
buffer_ref (Buffer *buffer)
{
  g_assert (buffer->refcount > 0);
  buffer->refcount++;
  return buffer;
}

G_DEFINE_AUTOPTR_CLEANUP_FUNC (Buffer, buffer_unref)

static void
free_side (ProxySide *side)
{
  g_clear_object (&side->connection);
  g_clear_pointer (&side->extra_input_data, g_bytes_unref);

  g_list_free_full (side->buffers, (GDestroyNotify) buffer_unref);
  g_list_free_full (side->control_messages, (GDestroyNotify) g_object_unref);

  if (side->in_source)
    g_source_destroy (side->in_source);
  if (side->out_source)
    g_source_destroy (side->out_source);

  g_hash_table_destroy (side->expected_replies);
}

static void
flatpak_proxy_client_finalize (GObject *object)
{
  FlatpakProxyClient *client = FLATPAK_PROXY_CLIENT (object);

  client->proxy->clients = g_list_remove (client->proxy->clients, client);
  g_clear_object (&client->proxy);

  g_hash_table_destroy (client->rewrite_reply);
  g_hash_table_destroy (client->get_owner_reply);
  g_hash_table_destroy (client->unique_id_policy);

  free_side (&client->client_side);
  free_side (&client->bus_side);

  G_OBJECT_CLASS (flatpak_proxy_client_parent_class)->finalize (object);
}

static void
flatpak_proxy_client_class_init (FlatpakProxyClientClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->finalize = flatpak_proxy_client_finalize;
}

static void
init_side (FlatpakProxyClient *client, ProxySide *side)
{
  side->got_first_byte = (side == &client->bus_side);
  side->client = client;
  side->header_buffer.size = 16;
  side->header_buffer.pos = 0;
  side->current_read_buffer = &side->header_buffer;
  side->expected_replies = g_hash_table_new (g_direct_hash, g_direct_equal);
}

static void
flatpak_proxy_client_init (FlatpakProxyClient *client)
{
  init_side (client, &client->client_side);
  init_side (client, &client->bus_side);

  client->auth_end_offset = AUTH_END_INIT_OFFSET;
  client->rewrite_reply = g_hash_table_new_full (g_direct_hash, g_direct_equal, NULL, g_object_unref);
  client->get_owner_reply = g_hash_table_new_full (g_direct_hash, g_direct_equal, NULL, g_free);
  client->unique_id_policy = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);
}

FlatpakProxyClient *
flatpak_proxy_client_new (FlatpakProxy *proxy, GSocketConnection *connection)
{
  FlatpakProxyClient *client;

  g_socket_set_blocking (g_socket_connection_get_socket (connection), FALSE);

  client = g_object_new (FLATPAK_TYPE_PROXY_CLIENT, NULL);
  client->proxy = g_object_ref (proxy);
  client->client_side.connection = g_object_ref (connection);

  proxy->clients = g_list_prepend (proxy->clients, client);

  return client;
}

static FlatpakPolicy
flatpak_proxy_get_wildcard_policy (FlatpakProxy *proxy,
                                   const char   *name)
{
  guint wildcard_policy = 0;
  char *dot;
  char buffer[256];

  dot = strrchr (name, '.');
  if (dot && (dot - name) <= 255)
    {
      strncpy (buffer, name, dot - name);
      buffer[dot - name] = 0;
      wildcard_policy = GPOINTER_TO_INT (g_hash_table_lookup (proxy->wildcard_policy, buffer));
    }

  return wildcard_policy;
}

FlatpakPolicy
flatpak_proxy_get_policy (FlatpakProxy *proxy,
                          const char   *name)
{
  guint policy, wildcard_policy;

  policy = GPOINTER_TO_INT (g_hash_table_lookup (proxy->policy, name));

  wildcard_policy = flatpak_proxy_get_wildcard_policy (proxy, name);

  return MAX (policy, wildcard_policy);
}

void
flatpak_proxy_set_filter (FlatpakProxy *proxy,
                          gboolean      filter)
{
  proxy->filter = filter;
}

void
flatpak_proxy_set_log_messages (FlatpakProxy *proxy,
                                gboolean      log)
{
  proxy->log_messages = log;
}

void
flatpak_proxy_add_policy (FlatpakProxy *proxy,
                          const char   *name,
                          FlatpakPolicy policy)
{
  g_hash_table_replace (proxy->policy, g_strdup (name), GINT_TO_POINTER (policy));
}

void
flatpak_proxy_add_wildcarded_policy (FlatpakProxy *proxy,
                                     const char   *name,
                                     FlatpakPolicy policy)
{
  g_hash_table_replace (proxy->wildcard_policy, g_strdup (name), GINT_TO_POINTER (policy));
}

static void
flatpak_proxy_finalize (GObject *object)
{
  FlatpakProxy *proxy = FLATPAK_PROXY (object);

  if (g_socket_service_is_active (G_SOCKET_SERVICE (proxy)))
    unlink (proxy->socket_path);

  g_clear_pointer (&proxy->dbus_address, g_free);
  g_assert (proxy->clients == NULL);

  g_hash_table_destroy (proxy->policy);
  g_hash_table_destroy (proxy->wildcard_policy);

  g_free (proxy->socket_path);
  g_free (proxy->dbus_address);

  G_OBJECT_CLASS (flatpak_proxy_parent_class)->finalize (object);
}

static void
flatpak_proxy_set_property (GObject      *object,
                            guint         prop_id,
                            const GValue *value,
                            GParamSpec   *pspec)
{
  FlatpakProxy *proxy = FLATPAK_PROXY (object);

  switch (prop_id)
    {
    case PROP_DBUS_ADDRESS:
      proxy->dbus_address = g_value_dup_string (value);
      break;

    case PROP_SOCKET_PATH:
      proxy->socket_path = g_value_dup_string (value);
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
    }
}

static void
flatpak_proxy_get_property (GObject    *object,
                            guint       prop_id,
                            GValue     *value,
                            GParamSpec *pspec)
{
  FlatpakProxy *proxy = FLATPAK_PROXY (object);

  switch (prop_id)
    {
    case PROP_DBUS_ADDRESS:
      g_value_set_string (value, proxy->dbus_address);
      break;

    case PROP_SOCKET_PATH:
      g_value_set_string (value, proxy->socket_path);
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
    }
}

static Buffer *
buffer_new (gsize size, Buffer *old)
{
  Buffer *buffer = g_malloc0 (sizeof (Buffer) + size - 16);

  buffer->control_messages = NULL;
  buffer->size = size;
  buffer->refcount = 1;

  if (old)
    {
      buffer->pos = old->pos;
      /* Takes ownership of any old control messages */
      buffer->control_messages = old->control_messages;
      old->control_messages = NULL;

      g_assert (size >= old->size);
      memcpy (buffer->data, old->data, old->size);
    }

  return buffer;
}

static ProxySide *
get_other_side (ProxySide *side)
{
  FlatpakProxyClient *client = side->client;

  if (side == &client->client_side)
    return &client->bus_side;

  return &client->client_side;
}

static void
side_closed (ProxySide *side)
{
  GSocket *socket, *other_socket;
  ProxySide *other_side = get_other_side (side);

  if (side->closed)
    return;

  socket = g_socket_connection_get_socket (side->connection);
  g_socket_close (socket, NULL);
  side->closed = TRUE;

  other_socket = g_socket_connection_get_socket (other_side->connection);
  if (!other_side->closed && other_side->buffers == NULL)
    {
      g_socket_close (other_socket, NULL);
      other_side->closed = TRUE;
    }

  if (other_side->closed)
    {
      g_object_unref (side->client);
    }
  else
    {
      GError *error = NULL;

      if (!g_socket_shutdown (other_socket, TRUE, FALSE, &error))
        {
          g_warning ("Unable to shutdown read side: %s", error->message);
          g_error_free (error);
        }
    }
}

static gboolean
buffer_read (ProxySide *side,
             Buffer    *buffer,
             GSocket   *socket)
{
  gssize res;
  GInputVector v;
  GError *error = NULL;
  GSocketControlMessage **messages;
  int num_messages, i;

  if (side->extra_input_data)
    {
      gsize extra_size;
      const guchar *extra_bytes = g_bytes_get_data (side->extra_input_data, &extra_size);

      res = MIN (extra_size, buffer->size - buffer->pos);
      memcpy (&buffer->data[buffer->pos], extra_bytes, res);

      if (res < extra_size)
        {
          side->extra_input_data =
            g_bytes_new_with_free_func (extra_bytes + res,
                                        extra_size - res,
                                        (GDestroyNotify) g_bytes_unref,
                                        side->extra_input_data);
        }
      else
        {
          g_clear_pointer (&side->extra_input_data, g_bytes_unref);
        }
    }
  else
    {
      int flags = 0;
      v.buffer = &buffer->data[buffer->pos];
      v.size = buffer->size - buffer->pos;

      res = g_socket_receive_message (socket, NULL, &v, 1,
                                      &messages,
                                      &num_messages,
                                      &flags, NULL, &error);
      if (res < 0 && g_error_matches (error, G_IO_ERROR, G_IO_ERROR_WOULD_BLOCK))
        {
          g_error_free (error);
          return FALSE;
        }

      if (res <= 0)
        {
          if (res != 0)
            {
              g_debug ("Error reading from socket: %s", error->message);
              g_error_free (error);
            }

          side_closed (side);
          return FALSE;
        }

      for (i = 0; i < num_messages; i++)
        buffer->control_messages = g_list_append (buffer->control_messages, messages[i]);

      g_free (messages);
    }

  buffer->pos += res;
  return TRUE;
}

static gboolean
buffer_write (ProxySide *side,
              Buffer    *buffer,
              GSocket   *socket)
{
  gssize res;
  GOutputVector v;
  GError *error = NULL;
  GSocketControlMessage **messages = NULL;
  int i, n_messages;
  GList *l;

  if (buffer->send_credentials &&
      G_IS_UNIX_CONNECTION (side->connection))
    {
      g_assert (buffer->size == 1);

      if (!g_unix_connection_send_credentials (G_UNIX_CONNECTION (side->connection),
                                               NULL,
                                               &error))
        {
          if (g_error_matches (error, G_IO_ERROR, G_IO_ERROR_WOULD_BLOCK))
            {
              g_error_free (error);
              return FALSE;
            }

          g_warning ("Error writing credentials to socket: %s", error->message);
          g_error_free (error);

          side_closed (side);
          return FALSE;
        }

      buffer->pos = 1;
      return TRUE;
    }

  n_messages = g_list_length (buffer->control_messages);
  messages = g_new (GSocketControlMessage *, n_messages);
  for (l = buffer->control_messages, i = 0; l != NULL; l = l->next, i++)
    messages[i] = l->data;

  v.buffer = &buffer->data[buffer->pos];
  v.size = buffer->size - buffer->pos;

  res = g_socket_send_message (socket, NULL, &v, 1,
                               messages, n_messages,
                               G_SOCKET_MSG_NONE, NULL, &error);
  g_free (messages);
  if (res < 0 && g_error_matches (error, G_IO_ERROR, G_IO_ERROR_WOULD_BLOCK))
    {
      g_error_free (error);
      return FALSE;
    }

  if (res <= 0)
    {
      if (res < 0)
        {
          g_warning ("Error writing credentials to socket: %s", error->message);
          g_error_free (error);
        }

      side_closed (side);
      return FALSE;
    }

  g_list_free_full (buffer->control_messages, g_object_unref);
  buffer->control_messages = NULL;

  buffer->pos += res;
  return TRUE;
}

static gboolean
side_out_cb (GSocket *socket, GIOCondition condition, gpointer user_data)
{
  ProxySide *side = user_data;
  FlatpakProxyClient *client = side->client;
  gboolean retval = G_SOURCE_CONTINUE;

  g_object_ref (client);

  while (side->buffers)
    {
      Buffer *buffer = side->buffers->data;

      if (buffer_write (side, buffer, socket))
        {
          if (buffer->pos == buffer->size)
            {
              side->buffers = g_list_delete_link (side->buffers, side->buffers);
              buffer_unref (buffer);
            }
        }
      else
        {
          break;
        }
    }

  if (side->buffers == NULL)
    {
      ProxySide *other_side = get_other_side (side);

      side->out_source = NULL;
      retval = G_SOURCE_REMOVE;

      if (other_side->closed)
        side_closed (side);
    }

  g_object_unref (client);

  return retval;
}

static void
queue_expected_reply (ProxySide *side, guint32 serial, ExpectedReplyType type)
{
  g_hash_table_replace (side->expected_replies,
                        GUINT_TO_POINTER (serial),
                        GUINT_TO_POINTER (type));
}

static ExpectedReplyType
steal_expected_reply (ProxySide *side, guint32 serial)
{
  ExpectedReplyType type;

  type = GPOINTER_TO_UINT (g_hash_table_lookup (side->expected_replies,
                                                GUINT_TO_POINTER (serial)));
  if (type)
    g_hash_table_remove (side->expected_replies,
                         GUINT_TO_POINTER (serial));
  return type;
}


static void
queue_outgoing_buffer (ProxySide *side, Buffer *buffer)
{
  if (side->out_source == NULL)
    {
      GSocket *socket;

      socket = g_socket_connection_get_socket (side->connection);
      side->out_source = g_socket_create_source (socket, G_IO_OUT, NULL);
      g_source_set_callback (side->out_source, (GSourceFunc) side_out_cb, side, NULL);
      g_source_attach (side->out_source, NULL);
      g_source_unref (side->out_source);
    }

  buffer->pos = 0;
  side->buffers = g_list_append (side->buffers, buffer);
}

static guint32
read_uint32 (Header *header, guint8 *ptr)
{
  if (header->big_endian)
    return GUINT32_FROM_BE (*(guint32 *) ptr);
  else
    return GUINT32_FROM_LE (*(guint32 *) ptr);
}

static void
write_uint32 (Header *header, guint8 *ptr, guint32 val)
{
  if (header->big_endian)
    *(guint32 *) ptr = GUINT32_TO_BE (val);
  else
    *(guint32 *) ptr = GUINT32_TO_LE (val);
}

static inline guint32
align_by_8 (guint32 offset)
{
  return (offset + 8 - 1) & ~(8 - 1);
}

static inline guint32
align_by_4 (guint32 offset)
{
  return (offset + 4 - 1) & ~(4 - 1);
}

static const char *
get_signature (Buffer *buffer, guint32 *offset, guint32 end_offset)
{
  guint8 len;
  char *str;

  if (*offset >= end_offset)
    return FALSE;

  len = buffer->data[*offset];
  (*offset)++;

  if ((*offset) + len + 1 > end_offset)
    return FALSE;

  if (buffer->data[(*offset) + len] != 0)
    return FALSE;

  str = (char *) &buffer->data[(*offset)];
  *offset += len + 1;

  return str;
}

static const char *
get_string (Buffer *buffer, Header *header, guint32 *offset, guint32 end_offset)
{
  guint8 len;
  char *str;

  *offset = align_by_4 (*offset);
  if (*offset + 4  >= end_offset)
    return FALSE;

  len = read_uint32 (header, &buffer->data[*offset]);
  *offset += 4;

  if ((*offset) + len + 1 > end_offset)
    return FALSE;

  if (buffer->data[(*offset) + len] != 0)
    return FALSE;

  str = (char *) &buffer->data[(*offset)];
  *offset += len + 1;

  return str;
}

static void
header_free (Header *header)
{
  if (header->buffer)
    buffer_unref (header->buffer);
  g_free (header);
}

static Header *
parse_header (Buffer *buffer, guint32 serial_offset, guint32 reply_serial_offset, guint32 hello_serial)
{
  guint32 array_len, header_len;
  guint32 offset, end_offset;
  guint8 header_type;
  guint32 reply_serial_pos = 0;
  const char *signature;
  g_autoptr(Header) header = g_new0 (Header, 1);

  header->buffer = buffer_ref (buffer);

  if (buffer->size < 16)
    return NULL;

  if (buffer->data[3] != 1) /* Protocol version */
    return NULL;

  if (buffer->data[0] == 'B')
    header->big_endian = TRUE;
  else if (buffer->data[0] == 'l')
    header->big_endian = FALSE;
  else
    return NULL;

  header->type = buffer->data[1];
  header->flags = buffer->data[2];

  header->length = read_uint32 (header, &buffer->data[4]);
  header->serial = read_uint32 (header, &buffer->data[8]);

  if (header->serial == 0)
    return NULL;

  array_len = read_uint32 (header, &buffer->data[12]);

  header_len = align_by_8 (12 + 4 + array_len);
  g_assert (buffer->size >= header_len); /* We should have verified this when reading in the message */
  if (header_len > buffer->size)
    return NULL;

  offset = 12 + 4;
  end_offset = offset + array_len;

  while (offset < end_offset)
    {
      offset = align_by_8 (offset); /* Structs must be 8 byte aligned */
      if (offset >= end_offset)
        return NULL;

      header_type = buffer->data[offset++];
      if (offset >= end_offset)
        return NULL;

      signature = get_signature (buffer, &offset, end_offset);
      if (signature == NULL)
        return NULL;

      switch (header_type)
        {
        case G_DBUS_MESSAGE_HEADER_FIELD_INVALID:
          return NULL;

        case G_DBUS_MESSAGE_HEADER_FIELD_PATH:
          if (strcmp (signature, "o") != 0)
            return NULL;
          header->path = get_string (buffer, header, &offset, end_offset);
          if (header->path == NULL)
            return NULL;
          break;

        case G_DBUS_MESSAGE_HEADER_FIELD_INTERFACE:
          if (strcmp (signature, "s") != 0)
            return NULL;
          header->interface = get_string (buffer, header, &offset, end_offset);
          if (header->interface == NULL)
            return NULL;
          break;

        case G_DBUS_MESSAGE_HEADER_FIELD_MEMBER:
          if (strcmp (signature, "s") != 0)
            return NULL;
          header->member = get_string (buffer, header, &offset, end_offset);
          if (header->member == NULL)
            return NULL;
          break;

        case G_DBUS_MESSAGE_HEADER_FIELD_ERROR_NAME:
          if (strcmp (signature, "s") != 0)
            return NULL;
          header->error_name = get_string (buffer, header, &offset, end_offset);
          if (header->error_name == NULL)
            return NULL;
          break;

        case G_DBUS_MESSAGE_HEADER_FIELD_REPLY_SERIAL:
          if (offset + 4 > end_offset)
            return NULL;

          header->has_reply_serial = TRUE;
          reply_serial_pos = offset;
          header->reply_serial = read_uint32 (header, &buffer->data[offset]);
          offset += 4;
          break;

        case G_DBUS_MESSAGE_HEADER_FIELD_DESTINATION:
          if (strcmp (signature, "s") != 0)
            return NULL;
          header->destination = get_string (buffer, header, &offset, end_offset);
          if (header->destination == NULL)
            return NULL;
          break;

        case G_DBUS_MESSAGE_HEADER_FIELD_SENDER:
          if (strcmp (signature, "s") != 0)
            return NULL;
          header->sender = get_string (buffer, header, &offset, end_offset);
          if (header->sender == NULL)
            return NULL;
          break;

        case G_DBUS_MESSAGE_HEADER_FIELD_SIGNATURE:
          if (strcmp (signature, "g") != 0)
            return NULL;
          header->signature = get_signature (buffer, &offset, end_offset);
          if (header->signature == NULL)
            return NULL;
          break;

        case G_DBUS_MESSAGE_HEADER_FIELD_NUM_UNIX_FDS:
          if (offset + 4 > end_offset)
            return NULL;

          header->unix_fds = read_uint32 (header, &buffer->data[offset]);
          offset += 4;
          break;

        default:
          /* Unknown header field, for safety, fail parse */
          return NULL;
        }
    }

  switch (header->type)
    {
    case G_DBUS_MESSAGE_TYPE_METHOD_CALL:
      if (header->path == NULL || header->member == NULL)
        return NULL;
      break;

    case G_DBUS_MESSAGE_TYPE_METHOD_RETURN:
      if (!header->has_reply_serial)
        return NULL;
      break;

    case G_DBUS_MESSAGE_TYPE_ERROR:
      if (header->error_name  == NULL || !header->has_reply_serial)
        return NULL;
      break;

    case G_DBUS_MESSAGE_TYPE_SIGNAL:
      if (header->path == NULL ||
          header->interface == NULL ||
          header->member == NULL)
        return NULL;
      if (strcmp (header->path, "/org/freedesktop/DBus/Local") == 0 ||
          strcmp (header->interface, "org.freedesktop.DBus.Local") == 0)
        return NULL;
      break;

    default:
      /* Unknown message type, for safety, fail parse */
      return NULL;
    }

  if (serial_offset > 0)
    {
      header->serial += serial_offset;
      write_uint32 (header, &buffer->data[8], header->serial);
    }

  if (reply_serial_offset > 0 &&
      header->has_reply_serial &&
      header->reply_serial > hello_serial + reply_serial_offset)
    write_uint32 (header, &buffer->data[reply_serial_pos], header->reply_serial - reply_serial_offset);

  return g_steal_pointer (&header);
}

static void
print_outgoing_header (Header *header)
{
  switch (header->type)
    {
    case G_DBUS_MESSAGE_TYPE_METHOD_CALL:
      g_print ("C%d: -> %s call %s.%s at %s\n",
               header->serial,
               header->destination ? header->destination : "(no dest)",
               header->interface ? header->interface : "",
               header->member ? header->member : "",
               header->path ? header->path : "");
      break;

    case G_DBUS_MESSAGE_TYPE_METHOD_RETURN:
      g_print ("C%d: -> %s return from B%d\n",
               header->serial,
               header->destination ? header->destination : "(no dest)",
               header->reply_serial);
      break;

    case G_DBUS_MESSAGE_TYPE_ERROR:
      g_print ("C%d: -> %s return error %s from B%d\n",
               header->serial,
               header->destination ? header->destination : "(no dest)",
               header->error_name ? header->error_name : "(no error)",
               header->reply_serial);
      break;

    case G_DBUS_MESSAGE_TYPE_SIGNAL:
      g_print ("C%d: -> %s signal %s.%s at %s\n",
               header->serial,
               header->destination ? header->destination : "all",
               header->interface ? header->interface : "",
               header->member ? header->member : "",
               header->path ? header->path : "");
      break;

    default:
      g_print ("unknown message type\n");
    }
}

static void
print_incoming_header (Header *header)
{
  switch (header->type)
    {
    case G_DBUS_MESSAGE_TYPE_METHOD_CALL:
      g_print ("B%d: <- %s call %s.%s at %s\n",
               header->serial,
               header->sender ? header->sender : "(no sender)",
               header->interface ? header->interface : "",
               header->member ? header->member : "",
               header->path ? header->path : "");
      break;

    case G_DBUS_MESSAGE_TYPE_METHOD_RETURN:
      g_print ("B%d: <- %s return from C%d\n",
               header->serial,
               header->sender ? header->sender : "(no sender)",
               header->reply_serial);
      break;

    case G_DBUS_MESSAGE_TYPE_ERROR:
      g_print ("B%d: <- %s return error %s from C%d\n",
               header->serial,
               header->sender ? header->sender : "(no sender)",
               header->error_name ? header->error_name : "(no error)",
               header->reply_serial);
      break;

    case G_DBUS_MESSAGE_TYPE_SIGNAL:
      g_print ("B%d: <- %s signal %s.%s at %s\n",
               header->serial,
               header->sender ? header->sender : "(no sender)",
               header->interface ? header->interface : "",
               header->member ? header->member : "",
               header->path ? header->path : "");
      break;

    default:
      g_print ("unknown message type\n");
    }
}

static FlatpakPolicy
flatpak_proxy_client_get_policy (FlatpakProxyClient *client, const char *source)
{
  if (source == NULL)
    return FLATPAK_POLICY_TALK; /* All clients can talk to the bus itself */

  if (source[0] == ':')
    return GPOINTER_TO_UINT (g_hash_table_lookup (client->unique_id_policy, source));

  return flatpak_proxy_get_policy (client->proxy, source);
}

static void
flatpak_proxy_client_update_unique_id_policy (FlatpakProxyClient *client,
                                              const char         *unique_id,
                                              FlatpakPolicy       policy)
{
  if (policy > FLATPAK_POLICY_NONE)
    {
      FlatpakPolicy old_policy;
      old_policy = GPOINTER_TO_UINT (g_hash_table_lookup (client->unique_id_policy, unique_id));
      if (policy > old_policy)
        g_hash_table_replace (client->unique_id_policy, g_strdup (unique_id), GINT_TO_POINTER (policy));
    }
}

static void
flatpak_proxy_client_update_unique_id_policy_from_name (FlatpakProxyClient *client,
                                                        const char         *unique_id,
                                                        const char         *as_name)
{
  flatpak_proxy_client_update_unique_id_policy (client,
                                                unique_id,
                                                flatpak_proxy_get_policy (client->proxy, as_name));
}


static gboolean
client_message_generates_reply (Header *header)
{
  switch (header->type)
    {
    case G_DBUS_MESSAGE_TYPE_METHOD_CALL:
      return (header->flags & G_DBUS_MESSAGE_FLAGS_NO_REPLY_EXPECTED) == 0;

    case G_DBUS_MESSAGE_TYPE_SIGNAL:
    case G_DBUS_MESSAGE_TYPE_METHOD_RETURN:
    case G_DBUS_MESSAGE_TYPE_ERROR:
    default:
      return FALSE;
    }
}

static Buffer *
message_to_buffer (GDBusMessage *message)
{
  Buffer *buffer;
  guchar *blob;
  gsize blob_size;

  blob = g_dbus_message_to_blob (message, &blob_size, G_DBUS_CAPABILITY_FLAGS_NONE, NULL);
  buffer = buffer_new (blob_size, NULL);
  memcpy (buffer->data, blob, blob_size);
  g_free (blob);

  return buffer;
}

static GDBusMessage *
get_error_for_header (FlatpakProxyClient *client, Header *header, const char *error)
{
  GDBusMessage *reply;

  reply = g_dbus_message_new ();
  g_dbus_message_set_message_type (reply, G_DBUS_MESSAGE_TYPE_ERROR);
  g_dbus_message_set_flags (reply, G_DBUS_MESSAGE_FLAGS_NO_REPLY_EXPECTED);
  g_dbus_message_set_reply_serial (reply, header->serial - client->serial_offset);
  g_dbus_message_set_error_name (reply, error);
  g_dbus_message_set_body (reply, g_variant_new ("(s)", error));

  return reply;
}

static GDBusMessage *
get_bool_reply_for_header (FlatpakProxyClient *client, Header *header, gboolean val)
{
  GDBusMessage *reply;

  reply = g_dbus_message_new ();
  g_dbus_message_set_message_type (reply, G_DBUS_MESSAGE_TYPE_METHOD_RETURN);
  g_dbus_message_set_flags (reply, G_DBUS_MESSAGE_FLAGS_NO_REPLY_EXPECTED);
  g_dbus_message_set_reply_serial (reply, header->serial - client->serial_offset);
  g_dbus_message_set_body (reply, g_variant_new_boolean (val));

  return reply;
}

static Buffer *
get_ping_buffer_for_header (Header *header)
{
  Buffer *buffer;
  GDBusMessage *dummy;

  dummy = g_dbus_message_new_method_call (NULL, "/", "org.freedesktop.DBus.Peer", "Ping");
  g_dbus_message_set_serial (dummy, header->serial);
  g_dbus_message_set_flags (dummy, header->flags);

  buffer = message_to_buffer (dummy);

  g_object_unref (dummy);

  return buffer;
}

static Buffer *
get_error_for_roundtrip (FlatpakProxyClient *client, Header *header, const char *error_name)
{
  Buffer *ping_buffer = get_ping_buffer_for_header (header);
  GDBusMessage *reply;

  reply = get_error_for_header (client, header, error_name);
  g_hash_table_replace (client->rewrite_reply, GINT_TO_POINTER (header->serial), reply);
  return ping_buffer;
}

static Buffer *
get_bool_reply_for_roundtrip (FlatpakProxyClient *client, Header *header, gboolean val)
{
  Buffer *ping_buffer = get_ping_buffer_for_header (header);
  GDBusMessage *reply;

  reply = get_bool_reply_for_header (client, header, val);
  g_hash_table_replace (client->rewrite_reply, GINT_TO_POINTER (header->serial), reply);

  return ping_buffer;
}

typedef enum {
  HANDLE_PASS,
  HANDLE_DENY,
  HANDLE_HIDE,
  HANDLE_FILTER_NAME_LIST_REPLY,
  HANDLE_FILTER_HAS_OWNER_REPLY,
  HANDLE_FILTER_GET_OWNER_REPLY,
  HANDLE_VALIDATE_OWN,
  HANDLE_VALIDATE_SEE,
  HANDLE_VALIDATE_TALK,
} BusHandler;

static gboolean
is_for_bus (Header *header)
{
  return g_strcmp0 (header->destination, "org.freedesktop.DBus") == 0;
}

static gboolean
is_dbus_method_call (Header *header)
{
  return
    is_for_bus (header) &&
    header->type == G_DBUS_MESSAGE_TYPE_METHOD_CALL &&
    g_strcmp0 (header->interface, "org.freedesktop.DBus") == 0;
}

static gboolean
is_introspection_call (Header *header)
{
  return
    header->type == G_DBUS_MESSAGE_TYPE_METHOD_CALL &&
    g_strcmp0 (header->interface, "org.freedesktop.DBus.Introspectable") == 0;
}

static BusHandler
get_dbus_method_handler (FlatpakProxyClient *client, Header *header)
{
  FlatpakPolicy policy;
  const char *method;

  if (header->has_reply_serial)
    {
      ExpectedReplyType expected_reply =
        steal_expected_reply (&client->bus_side,
                              header->reply_serial);
      if (expected_reply == EXPECTED_REPLY_NONE)
        return HANDLE_DENY;

      return HANDLE_PASS;
    }

  policy = flatpak_proxy_client_get_policy (client, header->destination);
  if (policy < FLATPAK_POLICY_SEE)
    return HANDLE_HIDE;
  if (policy < FLATPAK_POLICY_TALK)
    return HANDLE_DENY;

  if (!is_for_bus (header))
    return HANDLE_PASS;

  if (is_introspection_call (header))
    {
      return HANDLE_PASS;
    }
  else if (is_dbus_method_call (header))
    {
      method = header->member;
      if (method == NULL)
        return HANDLE_DENY;

      if (strcmp (method, "Hello") == 0 ||
          strcmp (method, "AddMatch") == 0 ||
          strcmp (method, "RemoveMatch") == 0 ||
          strcmp (method, "GetId") == 0)
        return HANDLE_PASS;

      if (strcmp (method, "UpdateActivationEnvironment") == 0 ||
          strcmp (method, "BecomeMonitor") == 0)
        return HANDLE_DENY;

      if (strcmp (method, "RequestName") == 0 ||
          strcmp (method, "ReleaseName") == 0 ||
          strcmp (method, "ListQueuedOwners") == 0)
        return HANDLE_VALIDATE_OWN;

      if (strcmp (method, "NameHasOwner") == 0)
        return HANDLE_FILTER_HAS_OWNER_REPLY;

      if (strcmp (method, "GetNameOwner") == 0)
        return HANDLE_FILTER_GET_OWNER_REPLY;

      if (strcmp (method, "GetConnectionUnixProcessID") == 0 ||
          strcmp (method, "GetConnectionCredentials") == 0 ||
          strcmp (method, "GetAdtAuditSessionData") == 0 ||
          strcmp (method, "GetConnectionSELinuxSecurityContext") == 0 ||
          strcmp (method, "GetConnectionUnixUser") == 0)
        return HANDLE_VALIDATE_SEE;

      if (strcmp (method, "StartServiceByName") == 0)
        return HANDLE_VALIDATE_TALK;

      if (strcmp (method, "ListNames") == 0 ||
          strcmp (method, "ListActivatableNames") == 0)
        return HANDLE_FILTER_NAME_LIST_REPLY;

      g_warning ("Unknown bus method %s\n", method);
      return HANDLE_DENY;
    }
  else
    {
      return HANDLE_DENY;
    }
}

static FlatpakPolicy
policy_from_handler (BusHandler handler)
{
  switch (handler)
    {
    case HANDLE_VALIDATE_OWN:
      return FLATPAK_POLICY_OWN;

    case HANDLE_VALIDATE_TALK:
      return FLATPAK_POLICY_TALK;

    case HANDLE_VALIDATE_SEE:
      return FLATPAK_POLICY_SEE;

    default:
      return FLATPAK_POLICY_NONE;
    }
}

static char *
get_arg0_string (Buffer *buffer)
{
  GDBusMessage *message = g_dbus_message_new_from_blob (buffer->data, buffer->size, 0, NULL);
  GVariant *body;
  g_autoptr(GVariant) arg0 = NULL;
  char *name = NULL;

  if (message != NULL &&
      (body = g_dbus_message_get_body (message)) != NULL &&
      (arg0 = g_variant_get_child_value (body, 0)) != NULL &&
      g_variant_is_of_type (arg0, G_VARIANT_TYPE_STRING))
    name = g_variant_dup_string (arg0, NULL);

  g_object_unref (message);

  return name;
}

static gboolean
validate_arg0_name (FlatpakProxyClient *client, Buffer *buffer, FlatpakPolicy required_policy, FlatpakPolicy *has_policy)
{
  GDBusMessage *message = g_dbus_message_new_from_blob (buffer->data, buffer->size, 0, NULL);
  GVariant *body, *arg0;
  const char *name;
  FlatpakPolicy name_policy;
  gboolean res = FALSE;

  if (has_policy)
    *has_policy = FLATPAK_POLICY_NONE;

  if (message != NULL &&
      (body = g_dbus_message_get_body (message)) != NULL &&
      (arg0 = g_variant_get_child_value (body, 0)) != NULL &&
      g_variant_is_of_type (arg0, G_VARIANT_TYPE_STRING))
    {
      name = g_variant_get_string (arg0, NULL);
      name_policy = flatpak_proxy_client_get_policy (client, name);

      if (has_policy)
        *has_policy = name_policy;

      if (name_policy >= required_policy)
        res = TRUE;
      else if (client->proxy->log_messages)
        g_print ("Filtering message due to arg0 %s, policy: %d (required %d)\n", name, name_policy, required_policy);
    }

  g_object_unref (message);
  return res;
}

static Buffer *
filter_names_list (FlatpakProxyClient *client, Buffer *buffer)
{
  GDBusMessage *message = g_dbus_message_new_from_blob (buffer->data, buffer->size, 0, NULL);
  GVariant *body, *arg0, *new_names;
  const gchar **names;
  int i;
  GVariantBuilder builder;
  Buffer *filtered;

  if (message == NULL ||
      (body = g_dbus_message_get_body (message)) == NULL ||
      (arg0 = g_variant_get_child_value (body, 0)) == NULL ||
      !g_variant_is_of_type (arg0, G_VARIANT_TYPE_STRING_ARRAY))
    return NULL;

  names = g_variant_get_strv (arg0, NULL);

  g_variant_builder_init (&builder, G_VARIANT_TYPE_STRING_ARRAY);
  for (i = 0; names[i] != NULL; i++)
    {
      if (flatpak_proxy_client_get_policy (client, names[i]) >= FLATPAK_POLICY_SEE)
        g_variant_builder_add (&builder, "s", names[i]);
    }
  g_free (names);

  new_names = g_variant_builder_end (&builder);
  g_dbus_message_set_body (message,
                           g_variant_new_tuple (&new_names, 1));

  filtered = message_to_buffer (message);
  g_object_unref (message);
  return filtered;
}

static gboolean
message_is_name_owner_changed (FlatpakProxyClient *client, Header *header)
{
  if (header->type == G_DBUS_MESSAGE_TYPE_SIGNAL &&
      g_strcmp0 (header->sender, "org.freedesktop.DBus") == 0 &&
      g_strcmp0 (header->interface, "org.freedesktop.DBus") == 0 &&
      g_strcmp0 (header->member, "NameOwnerChanged") == 0)
    return TRUE;
  return FALSE;
}

static gboolean
should_filter_name_owner_changed (FlatpakProxyClient *client, Buffer *buffer)
{
  GDBusMessage *message = g_dbus_message_new_from_blob (buffer->data, buffer->size, 0, NULL);
  GVariant *body, *arg0, *arg1, *arg2;
  const gchar *name, *old, *new;
  gboolean filter = TRUE;

  if (message == NULL ||
      (body = g_dbus_message_get_body (message)) == NULL ||
      (arg0 = g_variant_get_child_value (body, 0)) == NULL ||
      !g_variant_is_of_type (arg0, G_VARIANT_TYPE_STRING) ||
      (arg1 = g_variant_get_child_value (body, 1)) == NULL ||
      !g_variant_is_of_type (arg1, G_VARIANT_TYPE_STRING) ||
      (arg2 = g_variant_get_child_value (body, 2)) == NULL ||
      !g_variant_is_of_type (arg2, G_VARIANT_TYPE_STRING))
    return TRUE;

  name = g_variant_get_string (arg0, NULL);
  old = g_variant_get_string (arg1, NULL);
  new = g_variant_get_string (arg2, NULL);

  if (flatpak_proxy_client_get_policy (client, name) >= FLATPAK_POLICY_SEE)
    {
      if (name[0] != ':')
        {
          if (old[0] != 0)
            flatpak_proxy_client_update_unique_id_policy_from_name (client, old, name);

          if (new[0] != 0)
            flatpak_proxy_client_update_unique_id_policy_from_name (client, new, name);
        }

      filter = FALSE;
    }

  g_object_unref (message);

  return filter;
}

static GList *
side_get_n_unix_fds (ProxySide *side, int n_fds)
{
  GList *res = NULL;

  while (side->control_messages != NULL)
    {
      GSocketControlMessage *control_message = side->control_messages->data;

      if (G_IS_UNIX_FD_MESSAGE (control_message))
        {
          GUnixFDMessage *fd_message = G_UNIX_FD_MESSAGE (control_message);
          GUnixFDList *fd_list = g_unix_fd_message_get_fd_list (fd_message);
          int len = g_unix_fd_list_get_length (fd_list);

          /* I believe that socket control messages are never merged, and
             the sender side sends only one unix-fd-list per message, so
             at this point there should always be one full fd list
             per requested number of fds */
          if (len != n_fds)
            {
              g_warning ("Not right nr of fds in socket message");
              return NULL;
            }

          side->control_messages = g_list_delete_link (side->control_messages, side->control_messages);

          return g_list_append (NULL, control_message);
        }

      g_object_unref (control_message);
      side->control_messages = g_list_delete_link (side->control_messages, side->control_messages);
    }

  return res;
}

static gboolean
update_socket_messages (ProxySide *side, Buffer *buffer, Header *header)
{
  /* We may accidentally combine multiple control messages into one
     buffer when we receive (since we can do several recvs), so we
     keep a list of all we get and then only re-attach the amount
     specified in the header to the buffer. */

  side->control_messages = g_list_concat (side->control_messages, buffer->control_messages);
  buffer->control_messages = NULL;
  if (header->unix_fds > 0)
    {
      buffer->control_messages = side_get_n_unix_fds (side, header->unix_fds);
      if (buffer->control_messages == NULL)
        {
          g_warning ("Not enough fds for message");
          side_closed (side);
          buffer_unref (buffer);
          return FALSE;
        }
    }
  return TRUE;
}

static void
queue_fake_message (FlatpakProxyClient *client, GDBusMessage *message, ExpectedReplyType reply_type)
{
  Buffer *buffer;

  client->last_serial++;
  client->serial_offset++;
  g_dbus_message_set_serial (message, client->last_serial);
  buffer = message_to_buffer (message);
  g_object_unref (message);

  queue_outgoing_buffer (&client->bus_side, buffer);
  queue_expected_reply (&client->client_side, client->last_serial, reply_type);
}

/* After the first Hello message we need to synthesize a bunch of messages to synchronize the
   ownership state for the names in the policy */
static void
queue_initial_name_ops (FlatpakProxyClient *client)
{
  GHashTableIter iter;
  gpointer key, value;
  gboolean has_wildcards = FALSE;

  g_hash_table_iter_init (&iter, client->proxy->policy);
  while (g_hash_table_iter_next (&iter, &key, &value))
    {
      const char *name = key;
      GDBusMessage *message;
      GVariant *match;

      if (strcmp (name, "org.freedesktop.DBus") == 0)
        continue;

      /* AddMatch the name so we get told about ownership changes.
         Do it before the GetNameOwner to avoid races */
      message = g_dbus_message_new_method_call ("org.freedesktop.DBus", "/", "org.freedesktop.DBus", "AddMatch");
      match = g_variant_new_printf ("type='signal',sender='org.freedesktop.DBus',interface='org.freedesktop.DBus',member='NameOwnerChanged',arg0='%s'", name);
      g_dbus_message_set_body (message, g_variant_new_tuple (&match, 1));
      queue_fake_message (client, message, EXPECTED_REPLY_FILTER);

      if (client->proxy->log_messages)
        g_print ("C%d: -> org.freedesktop.DBus fake AddMatch for %s\n", client->last_serial, name);

      /* Get the current owner of the name (if any) so we can apply policy to it */
      message = g_dbus_message_new_method_call ("org.freedesktop.DBus", "/", "org.freedesktop.DBus", "GetNameOwner");
      g_dbus_message_set_body (message, g_variant_new ("(s)", name));
      queue_fake_message (client, message, EXPECTED_REPLY_FAKE_GET_NAME_OWNER);
      g_hash_table_replace (client->get_owner_reply, GINT_TO_POINTER (client->last_serial), g_strdup (name));

      if (client->proxy->log_messages)
        g_print ("C%d: -> org.freedesktop.DBus fake GetNameOwner for %s\n", client->last_serial, name);
    }

  /* Same for wildcard proxies. Only here we don't know the actual names to GetNameOwner for, so we have to
     list all current names */
  g_hash_table_iter_init (&iter, client->proxy->wildcard_policy);
  while (g_hash_table_iter_next (&iter, &key, &value))
    {
      const char *name = key;
      GDBusMessage *message;
      GVariant *match;

      has_wildcards = TRUE;

      /* AddMatch the name with arg0namespace so we get told about ownership changes to all subnames.
         Do it before the GetNameOwner to avoid races */
      message = g_dbus_message_new_method_call ("org.freedesktop.DBus", "/", "org.freedesktop.DBus", "AddMatch");
      match = g_variant_new_printf ("type='signal',sender='org.freedesktop.DBus',interface='org.freedesktop.DBus',member='NameOwnerChanged',arg0namespace='%s'", name);
      g_dbus_message_set_body (message, g_variant_new_tuple (&match, 1));
      queue_fake_message (client, message, EXPECTED_REPLY_FILTER);

      if (client->proxy->log_messages)
        g_print ("C%d: -> org.freedesktop.DBus fake AddMatch for %s.*\n", client->last_serial, name);
    }

  if (has_wildcards)
    {
      GDBusMessage *message;

      /* AddMatch the name so we get told about ownership changes.
         Do it before the GetNameOwner to avoid races */
      message = g_dbus_message_new_method_call ("org.freedesktop.DBus", "/", "org.freedesktop.DBus", "ListNames");
      g_dbus_message_set_body (message, g_variant_new ("()"));
      queue_fake_message (client, message, EXPECTED_REPLY_FAKE_LIST_NAMES);

      if (client->proxy->log_messages)
        g_print ("C%d: -> org.freedesktop.DBus fake ListNames\n", client->last_serial);

      /* Stop reading from the client, to avoid incoming messages fighting with the ListNames roundtrip.
         We will start it again once we have handled the ListNames reply */
      stop_reading (&client->client_side);
    }
}

static void
queue_wildcard_initial_name_ops (FlatpakProxyClient *client, Header *header, Buffer *buffer)
{
  GDBusMessage *decoded_message = g_dbus_message_new_from_blob (buffer->data, buffer->size, 0, NULL);
  GVariant *body, *arg0;

  if (decoded_message != NULL &&
      header->type == G_DBUS_MESSAGE_TYPE_METHOD_RETURN &&
      (body = g_dbus_message_get_body (decoded_message)) != NULL &&
      (arg0 = g_variant_get_child_value (body, 0)) != NULL &&
      g_variant_is_of_type (arg0, G_VARIANT_TYPE_STRING_ARRAY))
    {
      const gchar **names = g_variant_get_strv (arg0, NULL);
      int i;

      /* Loop over all current names and get the owner for all the ones that match our wildcard
         policies so that we can update the unique id policies for those */
      for (i = 0; names[i] != NULL; i++)
        {
          const char *name = names[i];

          if (name[0] != ':' &&
              flatpak_proxy_get_wildcard_policy (client->proxy, name) != FLATPAK_POLICY_NONE)
            {
              /* Get the current owner of the name (if any) so we can apply policy to it */
              GDBusMessage *message = g_dbus_message_new_method_call ("org.freedesktop.DBus", "/", "org.freedesktop.DBus", "GetNameOwner");
              g_dbus_message_set_body (message, g_variant_new ("(s)", name));
              queue_fake_message (client, message, EXPECTED_REPLY_FAKE_GET_NAME_OWNER);
              g_hash_table_replace (client->get_owner_reply, GINT_TO_POINTER (client->last_serial), g_strdup (name));

              if (client->proxy->log_messages)
                g_print ("C%d: -> org.freedesktop.DBus fake GetNameOwner for %s\n", client->last_serial, name);
            }
        }
      g_free (names);
    }

  g_object_unref (decoded_message);
}


static void
got_buffer_from_client (FlatpakProxyClient *client, ProxySide *side, Buffer *buffer)
{
  ExpectedReplyType expecting_reply = EXPECTED_REPLY_NONE;

  if (client->authenticated && client->proxy->filter)
    {
      g_autoptr(Header) header = NULL;;
      BusHandler handler;

      /* Filter and rewrite outgoing messages as needed */

      header = parse_header (buffer, client->serial_offset, 0, 0);
      if (header == NULL)
        {
          g_warning ("Invalid message header format");
          side_closed (side);
          buffer_unref (buffer);
          return;
        }

      if (!update_socket_messages (side, buffer, header))
        return;

      /* Make sure the client is not playing games with the serials, as that
         could confuse us. */
      if (header->serial <= client->last_serial)
        {
          g_warning ("Invalid client serial");
          side_closed (side);
          buffer_unref (buffer);
          return;
        }
      client->last_serial = header->serial;

      if (client->proxy->log_messages)
        print_outgoing_header (header);

      /* Keep track of the initial Hello request so that we can read
         the reply which has our assigned unique id */
      if (is_dbus_method_call (header) &&
          g_strcmp0 (header->member, "Hello") == 0)
        {
          expecting_reply = EXPECTED_REPLY_HELLO;
          client->hello_serial = header->serial;
        }

      handler = get_dbus_method_handler (client, header);

      switch (handler)
        {
        case HANDLE_FILTER_HAS_OWNER_REPLY:
        case HANDLE_FILTER_GET_OWNER_REPLY:
          if (!validate_arg0_name (client, buffer, FLATPAK_POLICY_SEE, NULL))
            {
              g_clear_pointer (&buffer, buffer_unref);
              if (handler == HANDLE_FILTER_GET_OWNER_REPLY)
                buffer = get_error_for_roundtrip (client, header,
                                                  "org.freedesktop.DBus.Error.NameHasNoOwner");
              else
                buffer = get_bool_reply_for_roundtrip (client, header, FALSE);

              expecting_reply = EXPECTED_REPLY_REWRITE;
              break;
            }

          goto handle_pass;

        case HANDLE_VALIDATE_OWN:
        case HANDLE_VALIDATE_SEE:
        case HANDLE_VALIDATE_TALK:
          {
            FlatpakPolicy name_policy;
            if (validate_arg0_name (client, buffer, policy_from_handler (handler), &name_policy))
              goto handle_pass;

            if (name_policy < (int) FLATPAK_POLICY_SEE)
              goto handle_hide;
            else
              goto handle_deny;
          }

        case HANDLE_FILTER_NAME_LIST_REPLY:
          expecting_reply = EXPECTED_REPLY_LIST_NAMES;
          goto handle_pass;

        case HANDLE_PASS:
handle_pass:
          if (client_message_generates_reply (header))
	    {
	      if (expecting_reply == EXPECTED_REPLY_NONE)
		expecting_reply = EXPECTED_REPLY_NORMAL;
	    }

          break;

        case HANDLE_HIDE:
handle_hide:
          g_clear_pointer (&buffer, buffer_unref);

          if (client_message_generates_reply (header))
            {
              const char *error;

              if (client->proxy->log_messages)
                g_print ("*HIDDEN* (ping)\n");

              if ((header->destination != NULL && header->destination[0] == ':') ||
                  (header->flags & G_DBUS_MESSAGE_FLAGS_NO_AUTO_START) != 0)
                error = "org.freedesktop.DBus.Error.NameHasNoOwner";
              else
                error = "org.freedesktop.DBus.Error.ServiceUnknown";

              buffer = get_error_for_roundtrip (client, header, error);
	      expecting_reply = EXPECTED_REPLY_REWRITE;
            }
          else
            {
              if (client->proxy->log_messages)
                g_print ("*HIDDEN*\n");
            }
          break;

        default:
        case HANDLE_DENY:
handle_deny:
          g_clear_pointer (&buffer, buffer_unref);

          if (client_message_generates_reply (header))
            {
              if (client->proxy->log_messages)
                g_print ("*DENIED* (ping)\n");

              buffer = get_error_for_roundtrip (client, header,
						"org.freedesktop.DBus.Error.AccessDenied");
	      expecting_reply = EXPECTED_REPLY_REWRITE;
            }
          else
            {
              if (client->proxy->log_messages)
                g_print ("*DENIED*\n");
            }
          break;
        }

      if (buffer != NULL && expecting_reply != EXPECTED_REPLY_NONE)
        queue_expected_reply (side, header->serial, expecting_reply);
    }

  if (buffer)
    queue_outgoing_buffer (&client->bus_side, buffer);

  if (buffer != NULL && expecting_reply == EXPECTED_REPLY_HELLO)
    queue_initial_name_ops (client);
}

static void
got_buffer_from_bus (FlatpakProxyClient *client, ProxySide *side, Buffer *buffer)
{
  if (client->authenticated && client->proxy->filter)
    {
      g_autoptr(Header) header = NULL;;
      GDBusMessage *rewritten;
      FlatpakPolicy policy;
      ExpectedReplyType expected_reply;

      /* Filter and rewrite incoming messages as needed */

      header = parse_header (buffer, 0, client->serial_offset, client->hello_serial);
      if (header == NULL)
        {
          g_warning ("Invalid message header format");
          buffer_unref (buffer);
          side_closed (side);
          return;
        }

      if (!update_socket_messages (side, buffer, header))
        return;

      if (client->proxy->log_messages)
        print_incoming_header (header);

      if (header->has_reply_serial)
        {
          expected_reply = steal_expected_reply (get_other_side (side), header->reply_serial);

          /* We only allow replies we expect */
          if (expected_reply == EXPECTED_REPLY_NONE)
            {
              if (client->proxy->log_messages)
                g_print ("*Unexpected reply*\n");
              buffer_unref (buffer);
              return;
            }

          switch (expected_reply)
            {
            case EXPECTED_REPLY_HELLO:
              /* When we get the initial reply to Hello, allow all
                 further communications to our own unique id. */
              if (header->type == G_DBUS_MESSAGE_TYPE_METHOD_RETURN)
                {
                  g_autofree char *my_id = get_arg0_string (buffer);
                  flatpak_proxy_client_update_unique_id_policy (client, my_id, FLATPAK_POLICY_TALK);
                  break;
                }

            case EXPECTED_REPLY_REWRITE:
              /* Replace a roundtrip ping with the rewritten message */

              rewritten = g_hash_table_lookup (client->rewrite_reply,
                                               GINT_TO_POINTER (header->reply_serial));

              if (client->proxy->log_messages)
                g_print ("*REWRITTEN*\n");

              g_dbus_message_set_serial (rewritten, header->serial);
              g_clear_pointer (&buffer, buffer_unref);
              buffer = message_to_buffer (rewritten);

              g_hash_table_remove (client->rewrite_reply,
                                   GINT_TO_POINTER (header->reply_serial));
              break;

            case EXPECTED_REPLY_FAKE_LIST_NAMES:
              /* This is a reply from the bus to a fake ListNames
                 request, request ownership of any name matching a
                 wildcard policy */

              queue_wildcard_initial_name_ops (client, header, buffer);

              /* Don't forward fake replies to the app */
              if (client->proxy->log_messages)
                g_print ("*SKIPPED*\n");
              g_clear_pointer (&buffer, buffer_unref);

              /* Start reading the clients requests now that we are done with the names */
              start_reading (&client->client_side);
              break;

            case EXPECTED_REPLY_FAKE_GET_NAME_OWNER:
              /* This is a reply from the bus to a fake GetNameOwner
                 request, update the policy for this unique name based on
                 the policy */
              {
                char *requested_name = g_hash_table_lookup (client->get_owner_reply, GINT_TO_POINTER (header->reply_serial));

                if (header->type == G_DBUS_MESSAGE_TYPE_METHOD_RETURN)
                  {
                    g_autofree char *owner = get_arg0_string (buffer);
                    flatpak_proxy_client_update_unique_id_policy_from_name (client, owner, requested_name);
                  }

                g_hash_table_remove (client->get_owner_reply, GINT_TO_POINTER (header->reply_serial));

                /* Don't forward fake replies to the app */
                if (client->proxy->log_messages)
                  g_print ("*SKIPPED*\n");
                g_clear_pointer (&buffer, buffer_unref);
                break;
              }

            case EXPECTED_REPLY_FILTER:
              if (client->proxy->log_messages)
                g_print ("*SKIPPED*\n");
              g_clear_pointer (&buffer, buffer_unref);
              break;

            case EXPECTED_REPLY_LIST_NAMES:
              /* This is a reply from the bus to a ListNames request, filter
                 it according to the policy */
              if (header->type == G_DBUS_MESSAGE_TYPE_METHOD_RETURN)
                {
                  Buffer *filtered_buffer;

                  filtered_buffer = filter_names_list (client, buffer);
                  g_clear_pointer (&buffer, buffer_unref);
                  buffer = filtered_buffer;
                }

              break;

            case EXPECTED_REPLY_NORMAL:
              break;

            default:
              g_warning ("Unexpected expected reply type %d\n", expected_reply);
            }
        }
      else /* Not reply */
        {

          /* Don't allow reply types with no reply_serial */
          if (header->type == G_DBUS_MESSAGE_TYPE_METHOD_RETURN ||
              header->type == G_DBUS_MESSAGE_TYPE_ERROR)
            {
              if (client->proxy->log_messages)
                g_print ("*Invalid reply*\n");
              g_clear_pointer (&buffer, buffer_unref);
            }

          /* We filter all NameOwnerChanged signal according to the policy */
	  if (message_is_name_owner_changed (client, header))
	    {
	      if (should_filter_name_owner_changed (client, buffer))
		g_clear_pointer (&buffer, buffer_unref);
	    }
	}

      /* All incoming broadcast signals are filtered according to policy */
      if (header->type == G_DBUS_MESSAGE_TYPE_SIGNAL && header->destination == NULL)
        {
          policy = flatpak_proxy_client_get_policy (client, header->sender);
          if (policy < FLATPAK_POLICY_TALK)
            {
              if (client->proxy->log_messages)
                g_print ("*FILTERED IN*\n");
              g_clear_pointer (&buffer, buffer_unref);
            }
        }

      /* We received and forwarded a message from a trusted peer. Make the policy for
         this unique id SEE so that the client can track its lifetime. */
      if (buffer && header->sender && header->sender[0] == ':')
        flatpak_proxy_client_update_unique_id_policy (client, header->sender, FLATPAK_POLICY_SEE);

      if (buffer && client_message_generates_reply (header))
        queue_expected_reply (side, header->serial, EXPECTED_REPLY_NORMAL);
    }

  if (buffer)
    queue_outgoing_buffer (&client->client_side, buffer);
}

static void
got_buffer_from_side (ProxySide *side, Buffer *buffer)
{
  FlatpakProxyClient *client = side->client;

  if (side == &client->client_side)
    got_buffer_from_client (client, side, buffer);
  else
    got_buffer_from_bus (client, side, buffer);
}

static gssize
find_auth_end (FlatpakProxyClient *client, Buffer *buffer)
{
  guchar *match;
  int i;

  /* First try to match any leftover at the start */
  if (client->auth_end_offset > 0)
    {
      gsize left = strlen (AUTH_END_STRING) - client->auth_end_offset;
      gsize to_match = MIN (left, buffer->pos);
      /* Matched at least up to to_match */
      if (memcmp (buffer->data, &AUTH_END_STRING[client->auth_end_offset], to_match) == 0)
        {
          client->auth_end_offset += to_match;

          /* Matched all */
          if (client->auth_end_offset == strlen (AUTH_END_STRING))
            return to_match;

          /* Matched to end of buffer */
          return -1;
        }

      /* Did not actually match at start */
      client->auth_end_offset = -1;
    }

  /* Look for whole match inside buffer */
  match = memmem (buffer, buffer->pos,
                  AUTH_END_STRING, strlen (AUTH_END_STRING));
  if (match != NULL)
    return match - buffer->data + strlen (AUTH_END_STRING);

  /* Record longest prefix match at the end */
  for (i = MIN (strlen (AUTH_END_STRING) - 1, buffer->pos); i > 0; i--)
    {
      if (memcmp (buffer->data + buffer->pos - i, AUTH_END_STRING, i) == 0)
        {
          client->auth_end_offset = i;
          break;
        }
    }

  return -1;
}

static gboolean
side_in_cb (GSocket *socket, GIOCondition condition, gpointer user_data)
{
  ProxySide *side = user_data;
  FlatpakProxyClient *client = side->client;
  GError *error = NULL;
  Buffer *buffer;
  gboolean retval = G_SOURCE_CONTINUE;

  g_object_ref (client);

  while (!side->closed)
    {
      if (!side->got_first_byte)
        buffer = buffer_new (1, NULL);
      else if (!client->authenticated)
        buffer = buffer_new (64, NULL);
      else
        buffer = side->current_read_buffer;

      if (!buffer_read (side, buffer, socket))
        {
          if (buffer != side->current_read_buffer)
              buffer_unref (buffer);
          break;
        }

      if (!client->authenticated)
        {
          if (buffer->pos > 0)
            {
              gboolean found_auth_end = FALSE;
              gsize extra_data;

              buffer->size = buffer->pos;
              if (!side->got_first_byte)
                {
                  buffer->send_credentials = TRUE;
                  side->got_first_byte = TRUE;
                }
              /* Look for end of authentication mechanism */
              else if (side == &client->client_side)
                {
                  gssize auth_end = find_auth_end (client, buffer);

                  if (auth_end >= 0)
                    {
                      found_auth_end = TRUE;
                      buffer->size = auth_end;
                      extra_data = buffer->pos - buffer->size;

                      /* We may have gotten some extra data which is not part of
                         the auth handshake, keep it for the next iteration. */
                      if (extra_data > 0)
                        side->extra_input_data = g_bytes_new (buffer->data + buffer->size, extra_data);
                    }
                }

              got_buffer_from_side (side, buffer);

              if (found_auth_end)
                client->authenticated = TRUE;
            }
          else
            {
              buffer_unref (buffer);
            }
        }
      else if (buffer->pos == buffer->size)
        {
          if (buffer == &side->header_buffer)
            {
              gssize required;
              required = g_dbus_message_bytes_needed (buffer->data, buffer->size, &error);
              if (required < 0)
                {
                  g_warning ("Invalid message header read");
                  side_closed (side);
                }
              else
                {
                  side->current_read_buffer = buffer_new (required, buffer);
                }
            }
          else
            {
              got_buffer_from_side (side, buffer);
              side->header_buffer.pos = 0;
              side->current_read_buffer = &side->header_buffer;
            }
        }
    }

  if (side->closed)
    {
      side->in_source = NULL;
      retval = G_SOURCE_REMOVE;
    }

  g_object_unref (client);

  return retval;
}

static void
start_reading (ProxySide *side)
{
  GSocket *socket;

  socket = g_socket_connection_get_socket (side->connection);
  side->in_source = g_socket_create_source (socket, G_IO_IN, NULL);
  g_source_set_callback (side->in_source, (GSourceFunc) side_in_cb, side, NULL);
  g_source_attach (side->in_source, NULL);
  g_source_unref (side->in_source);
}

static void
stop_reading (ProxySide *side)
{
  if (side->in_source)
    {
      g_source_destroy (side->in_source);
      side->in_source = NULL;
    }
}


static void
client_connected_to_dbus (GObject      *source_object,
                          GAsyncResult *res,
                          gpointer      user_data)
{
  FlatpakProxyClient *client = user_data;
  GSocketConnection *connection;
  GError *error = NULL;
  GIOStream *stream;

  stream = g_dbus_address_get_stream_finish (res, NULL, &error);
  if (stream == NULL)
    {
      g_warning ("Failed to connect to bus: %s\n", error->message);
      g_object_unref (client);
      return;
    }

  connection = G_SOCKET_CONNECTION (stream);
  g_socket_set_blocking (g_socket_connection_get_socket (connection), FALSE);
  client->bus_side.connection = connection;

  start_reading (&client->client_side);
  start_reading (&client->bus_side);
}

static gboolean
flatpak_proxy_incoming (GSocketService    *service,
                        GSocketConnection *connection,
                        GObject           *source_object)
{
  FlatpakProxy *proxy = FLATPAK_PROXY (service);
  FlatpakProxyClient *client;

  client = flatpak_proxy_client_new (proxy, connection);

  g_dbus_address_get_stream (proxy->dbus_address,
                             NULL,
                             client_connected_to_dbus,
                             client);
  return TRUE;
}

static void
flatpak_proxy_init (FlatpakProxy *proxy)
{
  proxy->policy = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);
  proxy->wildcard_policy = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);
  flatpak_proxy_add_policy (proxy, "org.freedesktop.DBus", FLATPAK_POLICY_TALK);
}

static void
flatpak_proxy_class_init (FlatpakProxyClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  GSocketServiceClass *socket_service_class = G_SOCKET_SERVICE_CLASS (klass);

  object_class->get_property = flatpak_proxy_get_property;
  object_class->set_property = flatpak_proxy_set_property;
  object_class->finalize = flatpak_proxy_finalize;

  socket_service_class->incoming = flatpak_proxy_incoming;

  g_object_class_install_property (object_class,
                                   PROP_DBUS_ADDRESS,
                                   g_param_spec_string ("dbus-address",
                                                        "",
                                                        "",
                                                        NULL,
                                                        G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY));
  g_object_class_install_property (object_class,
                                   PROP_SOCKET_PATH,
                                   g_param_spec_string ("socket-path",
                                                        "",
                                                        "",
                                                        NULL,
                                                        G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY));

}

FlatpakProxy *
flatpak_proxy_new (const char *dbus_address,
                   const char *socket_path)
{
  FlatpakProxy *proxy;

  proxy = g_object_new (FLATPAK_TYPE_PROXY, "dbus-address", dbus_address, "socket-path", socket_path, NULL);
  return proxy;
}

gboolean
flatpak_proxy_start (FlatpakProxy *proxy, GError **error)
{
  GSocketAddress *address;
  gboolean res;

  unlink (proxy->socket_path);

  address = g_unix_socket_address_new (proxy->socket_path);

  error = NULL;
  res = g_socket_listener_add_address (G_SOCKET_LISTENER (proxy),
                                       address,
                                       G_SOCKET_TYPE_STREAM,
                                       G_SOCKET_PROTOCOL_DEFAULT,
                                       NULL, /* source_object */
                                       NULL, /* effective_address */
                                       error);
  g_object_unref (address);

  if (!res)
    return FALSE;


  g_socket_service_start (G_SOCKET_SERVICE (proxy));
  return TRUE;
}

void
flatpak_proxy_stop (FlatpakProxy *proxy)
{
  unlink (proxy->socket_path);

  g_socket_service_stop (G_SOCKET_SERVICE (proxy));
}
