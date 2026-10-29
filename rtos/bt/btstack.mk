# BTstack sources compiled into the image (Classic: HCI, L2CAP, SDP, AVDTP/A2DP source,
# AVRCP target, SBC encoder, TLV link keys). The SBC decoder is only there because
# btstack_sbc_bluedroid.c carries encoder and decoder instances in one object. Shared by bt.mk and test/host_link.sh.
BTSTACK_SRC := \
	src/btstack_linked_list.c src/btstack_memory.c src/btstack_memory_pool.c src/btstack_run_loop.c \
	src/btstack_tlv.c src/btstack_util.c src/hci.c src/hci_cmd.c src/hci_dump.c src/l2cap.c \
	src/l2cap_signaling.c src/ad_parser.c \
	src/classic/a2dp.c src/classic/a2dp_source.c src/classic/avdtp.c src/classic/avdtp_acceptor.c \
	src/classic/avdtp_initiator.c src/classic/avdtp_source.c src/classic/avdtp_util.c \
	src/classic/avrcp.c src/classic/avrcp_target.c \
	src/classic/sdp_client.c src/classic/sdp_server.c src/classic/sdp_util.c \
	src/classic/btstack_link_key_db_tlv.c src/classic/btstack_sbc_bluedroid.c src/classic/btstack_sbc_plc.c \
	platform/posix/btstack_tlv_posix.c \
	3rd-party/bluedroid/encoder/srce/sbc_analysis.c 3rd-party/bluedroid/encoder/srce/sbc_dct.c \
	3rd-party/bluedroid/encoder/srce/sbc_dct_coeffs.c 3rd-party/bluedroid/encoder/srce/sbc_enc_bit_alloc_mono.c \
	3rd-party/bluedroid/encoder/srce/sbc_enc_bit_alloc_ste.c 3rd-party/bluedroid/encoder/srce/sbc_enc_coeffs.c \
	3rd-party/bluedroid/encoder/srce/sbc_encoder.c 3rd-party/bluedroid/encoder/srce/sbc_packing.c \
	3rd-party/bluedroid/decoder/srce/alloc.c 3rd-party/bluedroid/decoder/srce/bitalloc-sbc.c 3rd-party/bluedroid/decoder/srce/bitalloc.c 3rd-party/bluedroid/decoder/srce/bitstream-decode.c \
	3rd-party/bluedroid/decoder/srce/decoder-oina.c 3rd-party/bluedroid/decoder/srce/decoder-private.c 3rd-party/bluedroid/decoder/srce/decoder-sbc.c 3rd-party/bluedroid/decoder/srce/dequant.c \
	3rd-party/bluedroid/decoder/srce/framing-sbc.c 3rd-party/bluedroid/decoder/srce/framing.c 3rd-party/bluedroid/decoder/srce/oi_codec_version.c 3rd-party/bluedroid/decoder/srce/synthesis-8-generated.c \
	3rd-party/bluedroid/decoder/srce/synthesis-dct8.c 3rd-party/bluedroid/decoder/srce/synthesis-sbc.c
BTSTACK_INC := src platform/posix 3rd-party/bluedroid/encoder/include 3rd-party/bluedroid/decoder/include
# the port (rtos/bt)
NEXUS_BT_SRC := nexus_bt.c bt_audio.c btstack_run_loop_rtems.c hci_transport_aicwf.c

btstack-print:
	@echo $(BTSTACK_SRC)
btstack-print-inc:
	@echo $(BTSTACK_INC)
.PHONY: btstack-print btstack-print-inc
