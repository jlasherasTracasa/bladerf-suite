-- tb_rx_lab.vhd — Banco de pruebas del motor de espectro a través de rx_lab.vhd.
--
-- Inyecta las muestras I/Q de un fichero (una por cada 2 ciclos de rx_clock, como el
-- AD9361/ADI), con rx_mux = 3 y un canal habilitado, lee la FIFO de muestras como lo
-- haría el GPIF del FX3 y vuelca cada palabra de 32 bits (hex) a un fichero. El modelo
-- en C (firmware/model) compara el volcado exacto al bit.
library ieee;
    use ieee.std_logic_1164.all;
    use ieee.numeric_std.all;
    use std.textio.all;

library work;
    use work.fifo_readwrite_p.all;
    use work.bladerf_p.all;

entity tb_rx_lab is
    generic (
        LOG2N   : natural := 10;
        K       : natural := 2;
        PAD     : natural := 1;
        INFILE  : string  := "in.txt";
        OUTFILE : string  := "out.txt";
        NSAMP   : natural := 8192;     -- muestras del fichero de entrada
        EXTRA   : natural := 4096;     -- muestras a cero tras el fichero (vaciado)
        MUX     : natural := 3;        -- valor de rx_mux_sel
        RDGAP   : natural := 0;        -- 0: lectura continua; >0: como el GPIF: transacciones de 2048 palabras con RDGAP ciclos (100 MHz) de pausa
        STOCK   : natural := 0         -- 1: instancia rx.vhd de serie en lugar de rx_lab (solo MUX /= 3)
    );
end entity;

architecture sim of tb_rx_lab is
    constant RX_PERIOD : time := 8 ns;
    constant RD_PERIOD : time := 10 ns;

    signal rx_clock, rd_clock : std_logic := '0';
    signal rx_reset  : std_logic := '1';
    signal rx_enable : std_logic := '0';
    signal rx_timestamp : unsigned(63 downto 0) := (others => '0');

    signal adc_controls : sample_controls_t(0 to 1) := (others => SAMPLE_CONTROL_DISABLE);
    signal adc_streams  : sample_streams_t(0 to 1)  := (others => ZERO_SAMPLE);

    signal s_rdata : std_logic_vector(RX_FIFO_T_DEFAULT.rdata'range);
    signal s_rused : std_logic_vector(RX_FIFO_T_DEFAULT.rused'range);
    signal s_rempty, s_rfull, s_rreq : std_logic;
    signal m_rdata : std_logic_vector(META_FIFO_RX_T_DEFAULT.rdata'range);
    signal m_rused : std_logic_vector(META_FIFO_RX_T_DEFAULT.rused'range);
    signal m_rempty, m_rfull : std_logic;
    signal lb_wused : std_logic_vector(LOOPBACK_FIFO_T_DEFAULT.wused'range);
    signal lb_wfull : std_logic;
    signal lb_wenabled : std_logic;
    signal lb_wdata : std_logic_vector(LOOPBACK_FIFO_T_DEFAULT.wdata'range) := (others => '0');
    signal trig     : std_logic := 'Z';
    signal pkt_ready, ts_reset, ovf_led, trig_tb : std_logic;

    signal c_samples, c_ovf, c_fft, c_spec, c_late, c_words : unsigned(31 downto 0);
    signal spec_active : std_logic;
    signal done : boolean := false;
    signal s_rreq_g : std_logic := '0';
    function pad_bit(p : natural) return std_logic is begin if p = 1 then return '1'; else return '0'; end if; end function;
    constant pad_s : std_logic := pad_bit(PAD);
begin

    rx_clock <= not rx_clock after RX_PERIOD / 2;
    rd_clock <= not rd_clock after RD_PERIOD / 2;

    gen_stock : if STOCK = 1 generate
        dut_s : entity work.rx
            generic map (NUM_STREAMS => 2)
            port map (
                rx_reset => rx_reset, rx_clock => rx_clock, rx_enable => rx_enable,
                meta_en => '0', timestamp_reset => ts_reset, usb_speed => '0',
                rx_mux_sel => to_unsigned(MUX, 3), rx_overflow_led => ovf_led, rx_timestamp => rx_timestamp,
                trigger_arm => '0', trigger_fire => '0', trigger_master => '0', trigger_line => trig,
                trigger_signal_sync_tb => trig_tb,
                eight_bit_mode_en => '0', highly_packed_mode_en => '0',
                packet_en => '0', packet_control => PACKET_CONTROL_DEFAULT, packet_ready => pkt_ready,
                sample_fifo_rclock => rd_clock, sample_fifo_raclr => not rx_enable, sample_fifo_rreq => s_rreq,
                sample_fifo_rdata => s_rdata, sample_fifo_rempty => s_rempty, sample_fifo_rfull => s_rfull,
                sample_fifo_rused => s_rused,
                mini_exp => "00",
                meta_fifo_rclock => rd_clock, meta_fifo_raclr => not rx_enable, meta_fifo_rreq => '0',
                meta_fifo_rdata => m_rdata, meta_fifo_rempty => m_rempty, meta_fifo_rfull => m_rfull,
                meta_fifo_rused => m_rused,
                loopback_fifo_wenabled => lb_wenabled, loopback_fifo_wreset => rx_reset,
                loopback_fifo_wclock => rx_clock, loopback_fifo_wdata => lb_wdata, loopback_fifo_wreq => '0',
                loopback_fifo_wfull => lb_wfull, loopback_fifo_wused => lb_wused,
                adc_controls => adc_controls, adc_streams => adc_streams
            );
    end generate;

    gen_lab : if STOCK = 0 generate
        dut : entity work.rx_lab
            generic map (NUM_STREAMS => 2)
            port map (
                rx_reset => rx_reset, rx_clock => rx_clock, rx_enable => rx_enable,
                meta_en => '0', timestamp_reset => ts_reset, usb_speed => '0',
                rx_mux_sel => to_unsigned(MUX, 3), rx_overflow_led => ovf_led, rx_timestamp => rx_timestamp,
                trigger_arm => '0', trigger_fire => '0', trigger_master => '0', trigger_line => trig,
                trigger_signal_sync_tb => trig_tb,
                eight_bit_mode_en => '0', highly_packed_mode_en => '0',
                packet_en => '0', packet_control => PACKET_CONTROL_DEFAULT, packet_ready => pkt_ready,
                sample_fifo_rclock => rd_clock, sample_fifo_raclr => not rx_enable, sample_fifo_rreq => s_rreq,
                sample_fifo_rdata => s_rdata, sample_fifo_rempty => s_rempty, sample_fifo_rfull => s_rfull,
                sample_fifo_rused => s_rused,
                mini_exp => "00",
                meta_fifo_rclock => rd_clock, meta_fifo_raclr => not rx_enable, meta_fifo_rreq => '0',
                meta_fifo_rdata => m_rdata, meta_fifo_rempty => m_rempty, meta_fifo_rfull => m_rfull,
                meta_fifo_rused => m_rused,
                loopback_fifo_wenabled => lb_wenabled, loopback_fifo_wreset => rx_reset,
                loopback_fifo_wclock => rx_clock, loopback_fifo_wdata => lb_wdata, loopback_fifo_wreq => '0',
                loopback_fifo_wfull => lb_wfull, loopback_fifo_wused => lb_wused,
                adc_controls => adc_controls, adc_streams => adc_streams,
                spec_cfg_log2n => to_unsigned(LOG2N, 4), spec_cfg_k => to_unsigned(K, 16),
                spec_cfg_pad => pad_s, spec_cfg_clear => (others => '0'),
                c_samples => c_samples, c_fifo_ovf => c_ovf, c_fft_frames => c_fft,
                c_spec_frames => c_spec, c_spec_late => c_late, c_spec_words => c_words,
                spec_active => spec_active
            );
    end generate;

    -- Lector de la FIFO (como el GPIF: showahead, rdreq = not empty)
    s_rreq <= not s_rempty when RDGAP = 0 else s_rreq_g;
    reader : process(rd_clock)
        file f : text open write_mode is OUTFILE;
        variable l : line;
    begin
        if rising_edge(rd_clock) then
            if s_rreq = '1' and s_rempty = '0' and rx_enable = '1' then
                hwrite(l, s_rdata);
                writeline(f, l);
            end if;
        end if;
    end process;

    -- Lector tipo GPIF (solo si RDGAP > 0): espera usedw >= 2048 y lee 2048 palabras seguidas
    gpif_reader : process
    begin
        if RDGAP = 0 then wait; end if;
        s_rreq_g <= '0';
        loop
            wait until rising_edge(rd_clock) and rx_enable = '1' and unsigned(s_rused) >= 2048;
            for i in 0 to 2047 loop
                s_rreq_g <= '1';
                wait until rising_edge(rd_clock);
            end loop;
            s_rreq_g <= '0';
            for i in 1 to RDGAP loop wait until rising_edge(rd_clock); end loop;
        end loop;
    end process;

    -- Marca de tiempo: un tick por muestra (cada 2 ciclos), como el time_tamer
    ts_proc : process(rx_clock)
        variable t : std_logic := '0';
    begin
        if rising_edge(rx_clock) then
            if rx_enable = '1' then
                if t = '1' then rx_timestamp <= rx_timestamp + 1; end if;
                t := not t;
            end if;
        end if;
    end process;

    stim : process
        file f : text open read_mode is INFILE;
        variable l : line;
        variable a, b : integer;
        variable n : natural := 0;
    begin
        adc_controls(0).enable <= '1';
        adc_controls(0).data_req <= '1';
        wait for 100 ns;
        wait until rising_edge(rx_clock);
        rx_reset <= '0';
        for i in 0 to 19 loop wait until rising_edge(rx_clock); end loop;
        rx_enable <= '1';
        for i in 0 to 19 loop wait until rising_edge(rx_clock); end loop;
        while n < NSAMP + EXTRA loop
            if n < NSAMP then
                readline(f, l);
                read(l, a);
                read(l, b);
            else
                a := 0; b := 0;
            end if;
            adc_streams(0).data_i <= to_signed(a, 16);
            adc_streams(0).data_q <= to_signed(b, 16);
            adc_streams(0).data_v <= '1';
            wait until rising_edge(rx_clock);
            adc_streams(0).data_v <= '0';
            wait until rising_edge(rx_clock);
            n := n + 1;
        end loop;
        for i in 0 to 60000 loop wait until rising_edge(rx_clock); end loop;
        report "FIN: c_samples=" & integer'image(to_integer(c_samples)) &
               " fft=" & integer'image(to_integer(c_fft)) &
               " spec=" & integer'image(to_integer(c_spec)) &
               " late=" & integer'image(to_integer(c_late)) &
               " words=" & integer'image(to_integer(c_words)) &
               " ovf=" & integer'image(to_integer(c_ovf));
        done <= true;
        wait;
    end process;

    stop : process
    begin
        wait until done;
        wait for 1 us;
        std.env.stop;
    end process;

end architecture;
