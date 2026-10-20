import 'dart:async';
import 'dart:io';
import 'dart:typed_data';

/// The `.local` name this device answers to on the network, asked of its own
/// mDNS responder: a reverse (PTR) query for [address], sent straight to
/// that address's port 5353. iOS does not give apps the device's name
/// (gethostname says "localhost", the system resolver has no reverse
/// entry), but the responder answers a unicast question about itself.
Future<String?> mdnsReverseLookup(String address,
    {Duration timeout = const Duration(seconds: 2)}) async {
  final ip = InternetAddress.tryParse(address);
  if (ip == null || ip.type != InternetAddressType.IPv4) return null;
  RawDatagramSocket? socket;
  try {
    socket = await RawDatagramSocket.bind(InternetAddress.anyIPv4, 0);
    final name = '${address.split('.').reversed.join('.')}.in-addr.arpa';
    final answer = Completer<String?>();
    socket.listen((event) {
      if (event != RawSocketEvent.read || answer.isCompleted) return;
      final dg = socket!.receive();
      if (dg == null) return;
      final host = _ptrAnswer(dg.data);
      if (host != null) answer.complete(host);
    });
    socket.send(_ptrQuery(name), ip, 5353);
    return await answer.future.timeout(timeout, onTimeout: () => null);
  } catch (_) {
    return null;
  } finally {
    socket?.close();
  }
}

Uint8List _ptrQuery(String name) {
  final b = BytesBuilder();
  // id, flags, 1 question, no answers/authority/additional
  b.add([0x12, 0x34, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0]);
  for (final label in name.split('.')) {
    b.addByte(label.length);
    b.add(label.codeUnits);
  }
  // root, type PTR (12), class IN with the unicast-response bit
  b.add([0, 0, 12, 0x80, 1]);
  return b.toBytes();
}

// The first PTR record in the answer section, without the trailing dot.
String? _ptrAnswer(Uint8List m) {
  try {
    final data = ByteData.sublistView(m);
    final questions = data.getUint16(4);
    final answers = data.getUint16(6);
    var off = 12;
    for (var i = 0; i < questions; i++) {
      off = _readName(m, off).$2 + 4;
    }
    for (var i = 0; i < answers; i++) {
      off = _readName(m, off).$2;
      final type = data.getUint16(off);
      final rdlen = data.getUint16(off + 8);
      off += 10;
      if (type == 12) return _readName(m, off).$1;
      off += rdlen;
    }
  } catch (_) {}
  return null;
}

// A (possibly compressed) DNS name at [off]: the name and the offset after it.
(String, int) _readName(Uint8List m, int off) {
  final labels = <String>[];
  int? end;
  for (var hops = 0; hops < 32; hops++) {
    final len = m[off];
    if (len & 0xC0 == 0xC0) {
      end ??= off + 2;
      off = ((len & 0x3F) << 8) | m[off + 1];
      continue;
    }
    if (len == 0) {
      end ??= off + 1;
      break;
    }
    labels.add(String.fromCharCodes(m, off + 1, off + 1 + len));
    off += 1 + len;
  }
  return (labels.join('.'), end ?? off);
}
