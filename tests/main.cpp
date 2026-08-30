import support;
import test_at;
import test_modem;
import test_pdu;
import test_sms;

int main(int argc, char *argv[]) {
  tests::register_at_tests();
  tests::register_pdu_tests();
  tests::register_sms_tests();
  tests::register_modem_tests();
  return tests::run_tests(argc, const_cast<const char **>(argv)) ? 1 : 0;
}
